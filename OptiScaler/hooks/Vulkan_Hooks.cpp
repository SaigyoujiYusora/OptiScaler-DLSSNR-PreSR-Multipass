#include "pch.h"

#include "Vulkan_Hooks.h"
#include "Vulkan_Wsi.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>

#include <menu/menu_overlay_vk.h>
#include <proxies/KernelBase_Proxy.h>
#include <upscaler_time/UpscalerTime_Vk.h>

#include <misc/FrameLimit.h>
#include "Reflex_Hooks.h"

#include <spoofing/Vulkan_Spoofing.h>

#include <vulkan/vulkan.hpp>

#include <dlssnr/DlssNr_VkExtensions.h>
#include <dlssnr/DlssNrFinished_Vk.h>

#include <detours/detours.h>
#include <misc/IdentifyGpu.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <mutex>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "Hook_Utils.h"

// for menu rendering
static VkDevice _device = VK_NULL_HANDLE;
static HWND _hwnd = nullptr;

static std::mutex _vkPresentMutex;

PFN_vkCreateDevice o_vkCreateDevice = nullptr;
PFN_vkCreateInstance o_vkCreateInstance = nullptr;
PFN_vkCreateWin32SurfaceKHR o_vkCreateWin32SurfaceKHR = nullptr;
PFN_vkQueuePresentKHR o_QueuePresentKHR = nullptr;
PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR o_DestroySwapchainKHR = nullptr;
static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;
static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;

// Those aren't hooked, just grabbed for use
static PFN_vkGetPhysicalDeviceFeatures2 o_vkGetPhysicalDeviceFeatures2 = nullptr;
PFN_vkCreateSemaphore VulkanHooks::o_vkCreateSemaphore = nullptr;
PFN_vkSignalSemaphore VulkanHooks::o_vkSignalSemaphore = nullptr;
PFN_vkAntiLagUpdateAMD VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;

namespace
{
template <typename T> uint64_t HandleKey(T handle)
{
    if constexpr (std::is_pointer_v<T>)
        return reinterpret_cast<uint64_t>(handle);
    else
        return static_cast<uint64_t>(handle);
}

struct SwapchainState
{
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    bool verifiedWsi = false;
};

struct DeviceState
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    VulkanWsi::Dispatch dispatch;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport = nullptr;
    bool nativeLoaderChain = false;
    bool verifiedWsi = false;
    std::unordered_map<uint64_t, VulkanHooks::QueueInfo> queues;
    std::unordered_map<uint64_t, SwapchainState> swapchains;
};

std::mutex wsiMutex;
std::mutex wsiHookMutex;
std::unordered_map<uint64_t, DeviceState> deviceStates;
VulkanWsi::Dispatch installedWsi;
std::atomic_bool loaderResolverBound { false };

HMODULE ModuleFromAddress(PFN_vkVoidFunction function)
{
    if (!function)
        return nullptr;

    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(function), &module))
        return nullptr;
    return module;
}

std::wstring ModuleBaseName(HMODULE module)
{
    wchar_t path[MAX_PATH] = {};
    const auto length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (!length)
        return {};
    std::wstring name(path, length);
    const auto slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos)
        name.erase(0, slash + 1);
    std::transform(name.begin(), name.end(), name.begin(), [](wchar_t c) { return std::towlower(c); });
    return name;
}

void LogWsiFunction(const char* name, PFN_vkVoidFunction function)
{
    const auto module = ModuleFromAddress(function);
    wchar_t path[MAX_PATH] {};
    const auto length = module ? GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path))) : 0;
    MEMORY_BASIC_INFORMATION memory {};
    if (function)
        VirtualQuery(reinterpret_cast<LPCVOID>(function), &memory, sizeof(memory));
    LOG_INFO("Vulkan overlay WSI entry {}: address=0x{:X}, module='{}', base=0x{:X}, allocation=0x{:X}, type=0x{:X}",
             name, reinterpret_cast<uintptr_t>(function),
             length ? wstring_to_string(std::wstring(path, length)) : "<unknown>",
             reinterpret_cast<uintptr_t>(module), reinterpret_cast<uintptr_t>(memory.AllocationBase), memory.Type);
}

bool IsStreamlineWsi(PFN_vkVoidFunction function)
{
    if (!function)
        return true;
    const auto isProxyModule = [](HMODULE module)
    {
        if (!module)
            return true;
        const auto getExport = KernelBaseProxy::GetProcAddress_();
        return getExport(module, "slInit") != nullptr || getExport(module, "slGetPluginFunction") != nullptr;
    };
    if (isProxyModule(ModuleFromAddress(function)))
        return true;
    // Supplement the loader provenance check with recognition of known interposer/plugin targets.
    // This is not a general decoder or a claim that arbitrary third-party virtual swapchains are safe.
    const auto target = DetourCodeFromPointer(reinterpret_cast<PVOID>(function), nullptr);
    return target && target != reinterpret_cast<PVOID>(function) &&
           isProxyModule(ModuleFromAddress(reinterpret_cast<PFN_vkVoidFunction>(target)));
}
} // namespace

// Forward declaration
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo);
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain);
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator);

void VulkanHooks::RecordDevice(VkDevice device, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo)
{
    if (device == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || createInfo == nullptr)
        return;

    DeviceState recorded {};
    recorded.physicalDevice = physicalDevice;

    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    if (familyCount)
        vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    const auto getDeviceProc = o_vkGetDeviceProcAddr;
    const auto getQueue =
        getDeviceProc ? reinterpret_cast<PFN_vkGetDeviceQueue>(getDeviceProc(device, "vkGetDeviceQueue")) : nullptr;
    if (!getQueue)
    {
        LOG_WARN("Vulkan overlay queue record: vkGetDeviceQueue is unavailable");
        return;
    }

    for (uint32_t i = 0; i < createInfo->queueCreateInfoCount; ++i)
    {
        const auto& queueCreate = createInfo->pQueueCreateInfos[i];
        if (queueCreate.queueFamilyIndex >= families.size())
            continue;

        // vkGetDeviceQueue is only valid for the ordinary queue-create path. Protected or
        // otherwise flagged queues require a separate retrieval path and are not safe for this overlay.
        if (queueCreate.flags != 0)
            continue;

        for (uint32_t queueIndex = 0; queueIndex < queueCreate.queueCount; ++queueIndex)
        {
            VkQueue queue = VK_NULL_HANDLE;
            getQueue(device, queueCreate.queueFamilyIndex, queueIndex, &queue);
            if (queue == VK_NULL_HANDLE)
                continue;

            VulkanHooks::QueueInfo info {};
            info.queue = queue;
            info.familyIndex = queueCreate.queueFamilyIndex;
            info.queueIndex = queueIndex;
            info.flags = families[queueCreate.queueFamilyIndex].queueFlags;
            recorded.queues.emplace(HandleKey(queue), info);
        }
    }

    const auto instance = State::Instance().VulkanInstance;
    recorded.instance = instance;
    recorded.dispatch = VulkanWsi::Resolve(getDeviceProc, device);
    recorded.getSurfaceSupport = instance != VK_NULL_HANDLE && o_vkGetInstanceProcAddr
                                     ? reinterpret_cast<PFN_vkGetPhysicalDeviceSurfaceSupportKHR>(
                                           o_vkGetInstanceProcAddr(instance, "vkGetPhysicalDeviceSurfaceSupportKHR"))
                                     : nullptr;
    recorded.nativeLoaderChain =
        VulkanWsi::IsNativeChain(recorded.dispatch, loaderResolverBound.load(), IsStreamlineWsi);

    LOG_INFO("Vulkan overlay loader chain: device=0x{:X}, native={}, queues={}", HandleKey(device),
             recorded.nativeLoaderChain, recorded.queues.size());
    LogWsiFunction("Loader.CreateSwapchain", reinterpret_cast<PFN_vkVoidFunction>(recorded.dispatch.create));
    LogWsiFunction("Loader.QueuePresent", reinterpret_cast<PFN_vkVoidFunction>(recorded.dispatch.present));
    LogWsiFunction("Loader.GetSwapchainImages", reinterpret_cast<PFN_vkVoidFunction>(recorded.dispatch.images));
    std::lock_guard lock(wsiMutex);
    deviceStates[HandleKey(device)] = std::move(recorded);
}

bool VulkanHooks::GetDeviceContext(VkDevice device, VkPhysicalDevice& physicalDevice, VkInstance& instance)
{
    physicalDevice = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
    std::lock_guard lock(wsiMutex);
    const auto found = deviceStates.find(HandleKey(device));
    if (found == deviceStates.end() || !found->second.verifiedWsi)
        return false;
    physicalDevice = found->second.physicalDevice;
    instance = found->second.instance;
    return physicalDevice != VK_NULL_HANDLE && instance != VK_NULL_HANDLE;
}

bool VulkanHooks::GetGraphicsQueue(VkDevice device, QueueInfo& info)
{
    std::lock_guard lock(wsiMutex);
    const auto deviceIt = deviceStates.find(HandleKey(device));
    if (deviceIt == deviceStates.end())
        return false;

    for (const auto& [key, candidate] : deviceIt->second.queues)
    {
        if ((candidate.flags & VK_QUEUE_GRAPHICS_BIT) != 0)
        {
            info = candidate;
            return true;
        }
    }
    return false;
}

bool VulkanHooks::GetPresentQueue(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain, QueueInfo& info,
                                  bool* verifiedWsi)
{
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR getSurfaceSupport = nullptr;
    {
        std::lock_guard lock(wsiMutex);
        const auto deviceIt = deviceStates.find(HandleKey(device));
        if (deviceIt == deviceStates.end())
            return false;
        const auto queueIt = deviceIt->second.queues.find(HandleKey(queue));
        const auto swapchainIt = deviceIt->second.swapchains.find(HandleKey(swapchain));
        if (queueIt == deviceIt->second.queues.end() || swapchainIt == deviceIt->second.swapchains.end() ||
            !deviceIt->second.verifiedWsi || !swapchainIt->second.verifiedWsi)
            return false;
        info = queueIt->second;
        if (verifiedWsi)
            *verifiedWsi = swapchainIt->second.verifiedWsi;
        physicalDevice = deviceIt->second.physicalDevice;
        surface = swapchainIt->second.surface;
        getSurfaceSupport = deviceIt->second.getSurfaceSupport;
    }

    if ((info.flags & VK_QUEUE_GRAPHICS_BIT) == 0 || !getSurfaceSupport || surface == VK_NULL_HANDLE)
        return false;

    VkBool32 supported = VK_FALSE;
    if (getSurfaceSupport(physicalDevice, info.familyIndex, surface, &supported) != VK_SUCCESS || !supported)
        return false;
    return true;
}

PFN_vkGetSwapchainImagesKHR VulkanHooks::GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain,
                                                            bool* verifiedWsi)
{
    std::lock_guard lock(wsiMutex);
    const auto deviceIt = deviceStates.find(HandleKey(device));
    if (deviceIt == deviceStates.end())
        return nullptr;
    const auto swapchainIt = deviceIt->second.swapchains.find(HandleKey(swapchain));
    if (swapchainIt == deviceIt->second.swapchains.end())
        return nullptr;
    if (verifiedWsi)
        *verifiedWsi = swapchainIt->second.verifiedWsi;
    return deviceIt->second.verifiedWsi ? deviceIt->second.dispatch.images : nullptr;
}

bool VulkanHooks::IsVerifiedSwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    bool verified = false;
    GetSwapchainImages(device, swapchain, &verified);
    return verified;
}

void VulkanHooks::RecordSwapchain(VkDevice device, VkSwapchainKHR swapchain, VkSurfaceKHR surface)
{
    std::lock_guard lock(wsiMutex);
    const auto deviceIt = deviceStates.find(HandleKey(device));
    if (deviceIt == deviceStates.end())
        return;
    // Only a successful call through our bound raw CreateSwapchain hook can register an output.
    if (!deviceIt->second.verifiedWsi || swapchain == VK_NULL_HANDLE || surface == VK_NULL_HANDLE)
        return;
    deviceIt->second.swapchains[HandleKey(swapchain)] = { surface, false };
}

bool VulkanHooks::ConfirmSwapchainImages(VkDevice device, VkSwapchainKHR swapchain, const VkImage* images,
                                         uint32_t count)
{
    std::lock_guard lock(wsiMutex);
    const auto owner = deviceStates.find(HandleKey(device));
    if (owner == deviceStates.end() || !owner->second.verifiedWsi)
        return false;
    const auto found = owner->second.swapchains.find(HandleKey(swapchain));
    if (found == owner->second.swapchains.end())
        return false;
    auto& state = found->second;
    state.verifiedWsi = false;
    if (!images || count == 0)
        return false;
    for (uint32_t i = 0; i < count; ++i)
        if (images[i] == VK_NULL_HANDLE || std::find(images, images + i, images[i]) != images + i)
            return false;
    state.verifiedWsi = true;
    return true;
}

void VulkanHooks::ForgetSwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(wsiMutex);
    const auto deviceIt = deviceStates.find(HandleKey(device));
    if (deviceIt != deviceStates.end())
        deviceIt->second.swapchains.erase(HandleKey(swapchain));
}

void VulkanHooks::ForgetDevice(VkDevice device)
{
    std::lock_guard lock(wsiMutex);
    deviceStates.erase(HandleKey(device));

}

static void HookDevice(VkDevice InDevice)
{
    if (State::Instance().vulkanSkipHooks)
        return;
    std::lock_guard hookLock(wsiHookMutex);
    VulkanWsi::Dispatch dispatch;
    {
        std::lock_guard lock(wsiMutex);
        const auto found = deviceStates.find(HandleKey(InDevice));
        if (found == deviceStates.end() || !found->second.nativeLoaderChain)
            return;
        dispatch = found->second.dispatch;
    }

    if (!installedWsi.Complete())
    {
        o_CreateSwapchainKHR = dispatch.create;
        o_DestroySwapchainKHR = dispatch.destroy;
        o_QueuePresentKHR = dispatch.present;
        LONG result = DetourTransactionBegin();
        if (result != NO_ERROR)
        {
            o_CreateSwapchainKHR = nullptr;
            o_DestroySwapchainKHR = nullptr;
            o_QueuePresentKHR = nullptr;
            return;
        }
        PVOID createTarget = nullptr, destroyTarget = nullptr, presentTarget = nullptr;
        result = DetourUpdateThread(GetCurrentThread());
        if (result == NO_ERROR)
            result = DetourAttachEx(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR, nullptr, &createTarget, nullptr);
        if (result == NO_ERROR)
            result = DetourAttachEx(&(PVOID&) o_DestroySwapchainKHR, hkvkDestroySwapchainKHR, nullptr, &destroyTarget, nullptr);
        if (result == NO_ERROR)
            result = DetourAttachEx(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR, nullptr, &presentTarget, nullptr);
        if (result == NO_ERROR &&
            (IsStreamlineWsi(reinterpret_cast<PFN_vkVoidFunction>(createTarget)) ||
             IsStreamlineWsi(reinterpret_cast<PFN_vkVoidFunction>(destroyTarget)) ||
             IsStreamlineWsi(reinterpret_cast<PFN_vkVoidFunction>(presentTarget))))
            result = ERROR_INVALID_FUNCTION;
        if (result == NO_ERROR)
            result = DetourTransactionCommit();
        else
            DetourTransactionAbort();
        if (result != NO_ERROR)
        {
            o_CreateSwapchainKHR = nullptr;
            o_DestroySwapchainKHR = nullptr;
            o_QueuePresentKHR = nullptr;
            LOG_ERROR("Vulkan overlay: native WSI hook transaction failed: {:X}", result);
            return;
        }
        installedWsi = dispatch; // Preserve the original entry identities, not Detours trampolines.
    }

    const bool bound = dispatch.SameHooks(installedWsi);
    {
        std::lock_guard lock(wsiMutex);
        const auto found = deviceStates.find(HandleKey(InDevice));
        if (found != deviceStates.end())
            found->second.verifiedWsi = bound;
    }
    LOG_INFO("Vulkan overlay WSI boundary: {} native loader dispatch for device 0x{:X}",
             bound ? "bound" : "different hook targets; rejected", HandleKey(InDevice));
}

VALIDATE_HOOK(hkvkCreateWin32SurfaceKHR, PFN_vkCreateWin32SurfaceKHR)
static VkResult hkvkCreateWin32SurfaceKHR(VkInstance instance, const VkWin32SurfaceCreateInfoKHR* pCreateInfo,
                                          const VkAllocationCallbacks* pAllocator, VkSurfaceKHR* pSurface)
{
    LOG_FUNC();

    auto result = o_vkCreateWin32SurfaceKHR(instance, pCreateInfo, pAllocator, pSurface);

    auto procHwnd = Util::GetProcessWindow();
    LOG_DEBUG("procHwnd: {0:X}, swapchain hwnd: {1:X}", (UINT64) procHwnd, (UINT64) pCreateInfo->hwnd);

    if (result == VK_SUCCESS && !State::Instance().vulkanSkipHooks)
    {
        MenuOverlayVk::DestroyVulkanObjects(false);

        State::Instance().VulkanInstance = instance;
        LOG_DEBUG("Vulkan surface instance captured: {0:X}", (UINT64) instance);
        _hwnd = pCreateInfo->hwnd;
        LOG_DEBUG("_hwnd captured: {0:X}", (UINT64) _hwnd);
    }

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkCreateInstance, PFN_vkCreateInstance)
static VkResult hkvkCreateInstance(const VkInstanceCreateInfo* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                                   VkInstance* pInstance)
{
    LOG_FUNC();

    VkInstanceCreateInfo localCreateInfo {};
    memcpy(&localCreateInfo, pCreateInfo, sizeof(VkInstanceCreateInfo));

    VulkanSpoofing::hkvkCreateInstance(&localCreateInfo, pAllocator, pInstance);

    VkResult result;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_vkCreateInstance(&localCreateInfo, pAllocator, pInstance);
    }

    if (result == VK_SUCCESS)
    {
        State::Instance().VulkanInstance = *pInstance;
        LOG_DEBUG("State::Instance().VulkanInstance captured: {0:X}", (UINT64) State::Instance().VulkanInstance);

#ifdef VULKAN_DEBUG_LAYER
        auto address = vkGetInstanceProcAddr(State::Instance().VulkanInstance, "vkCreateDebugUtilsMessengerEXT");
        auto vkCreateDebugUtilsMessengerEXT = (PFN_vkCreateDebugUtilsMessengerEXT) address;
        VkDebugUtilsMessengerEXT debugMessenger;
        vkCreateDebugUtilsMessengerEXT(State::Instance().VulkanInstance, &VulkanSpoofing::debugCreateInfo, nullptr,
                                       &debugMessenger);
#endif
    }

    // Disabled to prevent unnecessary object release
    // if (result == VK_SUCCESS && !State::Instance().vulkanSkipHooks)
    //{
    //     MenuOverlayVk::DestroyVulkanObjects(false);
    // }

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkCreateDevice, PFN_vkCreateDevice)
static VkResult hkvkCreateDevice(VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* pCreateInfo,
                                 const VkAllocationCallbacks* pAllocator, VkDevice* pDevice)
{
    LOG_FUNC();

    VkDeviceCreateInfo localCreteInfo {};
    memcpy(&localCreteInfo, pCreateInfo, sizeof(VkDeviceCreateInfo));

    // Check support for AntiLag before spoof
    VkPhysicalDeviceFeatures2 features2 = {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;

    VkPhysicalDeviceAntiLagFeaturesAMD antiLagFeatures = {};
    antiLagFeatures.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ANTI_LAG_FEATURES_AMD;

    features2.pNext = &antiLagFeatures;

    if (o_vkGetPhysicalDeviceFeatures2)
    {
        o_vkGetPhysicalDeviceFeatures2(physicalDevice, &features2);
        State::Instance().vkAntiLagSupported = antiLagFeatures.antiLag != 0;
    }

    VulkanSpoofing::hkvkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    // Neural Rendering on Vulkan without a D3D12 bridge, or the reason it cannot be.
    //
    // The model needs two NVIDIA vendor extensions to load its kernels, a game never asks for them,
    // and a device's extension list cannot be changed after creation. This is the only moment it can
    // be arranged. Reported either way: if the answer is no, the log says so here rather than leaving
    // a create failure three layers down to be explained.
    //
    // Only appended when the feature is switched on, and only what the physical device already
    // offers -- asking for an extension a driver does not have makes vkCreateDevice fail and the game
    // not start.
    DlssNr::VkExt::Merged nrExtensions;

    if (Config::Instance()->DlssNrEnabled.value_or_default())
    {
        const auto supported = DlssNr::VkExt::SupportedDeviceExtensions(
            o_vkGetInstanceProcAddr, State::Instance().VulkanInstance, physicalDevice);

        nrExtensions.names.assign(localCreteInfo.ppEnabledExtensionNames,
                                  localCreteInfo.ppEnabledExtensionNames + localCreteInfo.enabledExtensionCount);

        std::string present, added, missing;

        for (const char* want : DlssNr::VkExt::kDevice)
        {
            const bool already = DlssNr::VkExt::ListHas(localCreteInfo.ppEnabledExtensionNames,
                                                        localCreteInfo.enabledExtensionCount, want);

            if (already)
                present += std::string(present.empty() ? "" : ", ") + want;
            else if (!DlssNr::VkExt::Contains(supported, want))
                missing += std::string(missing.empty() ? "" : ", ") + want;
            else
            {
                nrExtensions.names.push_back(want);
                added += std::string(added.empty() ? "" : ", ") + want;
            }
        }

        LOG_INFO("DLSS-NR Vulkan: device offers {} extensions. game already enabled: [{}]. added here: "
                 "[{}]. NOT AVAILABLE: [{}]",
                 supported.size(), present.empty() ? "none" : present, added.empty() ? "none" : added,
                 missing.empty() ? "none" : missing);

        if (!missing.empty())
            LOG_WARN("DLSS-NR Vulkan: the native path is not possible on this device -- the model's kernels "
                     "cannot be loaded without the extensions listed as NOT AVAILABLE");

        if (!added.empty())
        {
            localCreteInfo.ppEnabledExtensionNames = nrExtensions.names.data();
            localCreteInfo.enabledExtensionCount = (uint32_t) nrExtensions.names.size();
        }
    }

    auto result = o_vkCreateDevice(physicalDevice, &localCreteInfo, pAllocator, pDevice);

    if (Config::Instance()->DlssNrEnabled.value_or_default())
        LOG_INFO("DLSS-NR Vulkan: vkCreateDevice returned {} with {} extensions requested", (int) result,
                 localCreteInfo.enabledExtensionCount);

    if (result == VK_SUCCESS && Config::Instance()->OverlayMenu.value_or_default())
    {
        if (!State::Instance().vulkanSkipHooks)
        {
            VulkanHooks::RecordDevice(*pDevice, physicalDevice, pCreateInfo);

            // Disabled to prevent unnecessary object release
            // MenuOverlayVk::DestroyVulkanObjects(false);

            LOG_DEBUG("Vulkan physical device captured: {0:X}", (UINT64) physicalDevice);
            _device = *pDevice;
            LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);
            HookDevice(_device);
        }

        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};

        VkPhysicalDeviceIDProperties idProps {};
        idProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES;

        VkPhysicalDeviceProperties2 props2 {};
        props2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        props2.pNext = &idProps;

        vkGetPhysicalDeviceProperties2(physicalDevice, &props2);

        if (idProps.deviceLUIDValid == VK_TRUE)
        {
            auto primaryGpu = IdentifyGpu::getPrimaryGpu();
            auto luid = (PLUID) idProps.deviceLUID;
            if (!IsEqualLUID(*luid, primaryGpu.luid))
                LOG_WARN("VkDevice created with non-primary GPU");
        }
    }

    if (State::Instance().vkAntiLagSupported)
    {
        if (result == VK_SUCCESS && o_vkGetDeviceProcAddr)
        {
            VulkanHooks::o_vkAntiLagUpdateAMD =
                (PFN_vkAntiLagUpdateAMD) o_vkGetDeviceProcAddr(*pDevice, "vkAntiLagUpdateAMD");
        }
        else
        {
            State::Instance().vkAntiLagSupported = false;
            LOG_WARN("Vulkan AntiLag can't be enabled");
        }
    }

#ifdef USE_QUEUE_SUBMIT_2_KHR
    if (result == VK_SUCCESS)
        hkvkGetDeviceProcAddr(*pDevice, "vkQueueSubmit2KHR");
#endif

    LOG_FUNC_RESULT(result);

    return result;
}

VALIDATE_HOOK(hkvkQueuePresentKHR, PFN_vkQueuePresentKHR)
static VkResult hkvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();
    MenuOverlayVk::PresentScope presentScope;

    // get upscaler time
    UpscalerTimeVk::ReadUpscalingTime(_device);

    // ??? TODO: if we are hooking dxvk's vulkan calls then this present call could be either coming from dxvk or from a
    // native vk game
    if (!IdentifyGpu::getPrimaryGpu().usesDxvk)
        State::Instance().swapchainApi = Vulkan;

    // Tick feature to let it know if it's frozen
    if (auto currentFeature = State::Instance().currentFeature; currentFeature != nullptr)
    {
        if (auto currentFg = State::Instance().currentFG; currentFg != nullptr)
            currentFeature->TickFrozenCheck(currentFg->GetInterpolatedFrameCount());
        else
            currentFeature->TickFrozenCheck();
    }

    VkPresentInfoKHR localPresentInfo {};
    memcpy(&localPresentInfo, pPresentInfo, sizeof(VkPresentInfoKHR));

    DlssNr::FinishedVkPresent(queue, &localPresentInfo);

    // render menu if needed
    MenuOverlayVk::QueuePresent(queue, &localPresentInfo);

    ReflexHooks::update(false, true);

    // original call
    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    auto result = o_QueuePresentKHR(queue, &localPresentInfo);

    // Unsure about Vulkan Reflex fps limit and if that could be causing an issue here
    if (!State::Instance().reflexLimitsFps)
        FrameLimit::sleep(false);

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkCreateSwapchainKHR, PFN_vkCreateSwapchainKHR)
static VkResult hkvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* pCreateInfo,
                                       const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    const bool boundDevice = VulkanHooks::GetDeviceContext(device, physicalDevice, instance);
    VkSwapchainCreateInfoKHR nrCreateInfo = *pCreateInfo;
    const bool prepareNr = Config::Instance()->DlssNrEnabled.value_or_default();
    const bool prepareOverlay =
        Config::Instance()->OverlayMenu.value_or_default() && !State::Instance().vulkanSkipHooks;
    if (prepareNr || prepareOverlay)
    {
        VkSurfaceCapabilitiesKHR capabilities {};
        if (boundDevice &&
            vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice, pCreateInfo->surface, &capabilities) == VK_SUCCESS)
        {
            if (prepareNr)
                nrCreateInfo.imageUsage |= capabilities.supportedUsageFlags &
                                           (VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT);
            if (prepareOverlay)
                nrCreateInfo.imageUsage |= capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        }
        pCreateInfo = &nrCreateInfo;
    }
    ScopedVulkanCreatingSC scopedVulkanCreatingSC {};
    VkResult result = VK_SUCCESS;
    {
        ScopedSkipSpoofingGlobal skipSpoofingGlobal {};
        result = o_CreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    }

    if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && *pSwapchain != VK_NULL_HANDLE &&
        !State::Instance().vulkanSkipHooks && boundDevice)
    {
        VulkanHooks::RecordSwapchain(device, *pSwapchain, pCreateInfo->surface);

        if (prepareNr)
            DlssNr::FinishedVkSwapchain(device, *pSwapchain, *pCreateInfo);
        State::Instance().screenWidth = static_cast<float>(pCreateInfo->imageExtent.width);
        State::Instance().screenHeight = static_cast<float>(pCreateInfo->imageExtent.height);

        // The same question the DXGI side asks: what does one unit of this buffer mean?
        //
        // EXTENDED_SRGB_LINEAR is scRGB, 1.0 = 80 nits. HDR10_ST2084 is PQ, 1.0 = 10000 nits. Both
        // are absolute, so in either the white point is arithmetic rather than a reading -- which
        // matters most for the games that supply no exposure texture, since nothing else answers for
        // them. Logged, not yet used.
        {
            static VkColorSpaceKHR lastSpace = (VkColorSpaceKHR) -1;

            if (pCreateInfo->imageColorSpace != lastSpace)
            {
                lastSpace = pCreateInfo->imageColorSpace;

                const char* name = "other";
                const char* meaning = "relative -- no scale to be had";

                switch (pCreateInfo->imageColorSpace)
                {
                case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
                    name = "scRGB (extended sRGB, linear)";
                    meaning = "absolute: 1.0 = 80 nits, so 203-nit paper white = 2.5375";
                    break;
                case VK_COLOR_SPACE_HDR10_ST2084_EXT:
                    name = "PQ / ST.2084 (HDR10)";
                    meaning = "absolute: 1.0 = 10000 nits, so 203-nit paper white = 0.0203";
                    break;
                case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR:
                    name = "sRGB (SDR)";
                    break;
                case VK_COLOR_SPACE_HDR10_HLG_EXT:
                    name = "HLG";
                    break;
                default:
                    break;
                }

                LOG_INFO("DLSS-NR: swapchain colour space {} -- {} ({}), format {}", (int) pCreateInfo->imageColorSpace,
                         name, meaning, (int) pCreateInfo->imageFormat);
            }
        }

        LOG_DEBUG("if (result == VK_SUCCESS && device != VK_NULL_HANDLE && pCreateInfo != nullptr && pSwapchain != "
                  "VK_NULL_HANDLE)");

        _device = device;
        LOG_DEBUG("_device captured: {0:X}", (UINT64) _device);

        MenuOverlayVk::CreateSwapchain(device, physicalDevice, instance, _hwnd, pCreateInfo, pAllocator, pSwapchain);
    }

    LOG_FUNC_RESULT(result);
    return result;
}

VALIDATE_HOOK(hkvkDestroySwapchainKHR, PFN_vkDestroySwapchainKHR)
static void hkvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain, const VkAllocationCallbacks* pAllocator)
{
    MenuOverlayVk::DestroySwapchain(device, swapchain);
    VulkanHooks::ForgetSwapchain(device, swapchain);
    o_DestroySwapchainKHR(device, swapchain, pAllocator);
}

VALIDATE_HOOK(hkvkGetInstanceProcAddr, PFN_vkGetInstanceProcAddr)
PFN_vkVoidFunction hkvkGetInstanceProcAddr(VkInstance instance, const char* pName)
{
    auto orgFunc = o_vkGetInstanceProcAddr(instance, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }

    auto result = VulkanSpoofing::hkvkGetInstanceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

VALIDATE_HOOK(hkvkGetDeviceProcAddr, PFN_vkGetDeviceProcAddr)
PFN_vkVoidFunction hkvkGetDeviceProcAddr(VkDevice device, const char* pName)
{
    auto orgFunc = o_vkGetDeviceProcAddr(device, pName);

    if (orgFunc == VK_NULL_HANDLE)
        return VK_NULL_HANDLE;

    auto procName = std::string(pName);

    if (procName == std::string("vkCreateInstance"))
    {
        if (o_vkCreateInstance == nullptr)
            o_vkCreateInstance = (PFN_vkCreateInstance) orgFunc;

        LOG_DEBUG("vkCreateInstance");
        return (PFN_vkVoidFunction) hkvkCreateInstance;
    }
    else if (procName == std::string("vkCreateDevice"))
    {
        if (o_vkCreateDevice == nullptr)
            o_vkCreateDevice = (PFN_vkCreateDevice) orgFunc;

        LOG_DEBUG("vkCreateDevice");
        return (PFN_vkVoidFunction) hkvkCreateDevice;
    }

    auto result = VulkanSpoofing::hkvkGetDeviceProcAddr(orgFunc, pName);
    if (result != VK_NULL_HANDLE)
        return result;

    return orgFunc;
}

void VulkanHooks::Hook(HMODULE vulkan1)
{
    if (vulkanModule == nullptr)
        vulkanModule = vulkan1;

    VulkanSpoofing::HookForVulkanSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanExtensionSpoofing(vulkan1);
    VulkanSpoofing::HookForVulkanVRAMSpoofing(vulkan1);

    if (o_vkCreateDevice != nullptr)
        return;

    FARPROC address = nullptr;

    o_vkCreateDevice = (PFN_vkCreateDevice) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateDevice");
    o_vkCreateInstance = (PFN_vkCreateInstance) KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateInstance");

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetInstanceProcAddr");
    o_vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetDeviceProcAddr");
    o_vkGetDeviceProcAddr = (PFN_vkGetDeviceProcAddr) address;
    const bool loaderOwned = ModuleFromAddress(reinterpret_cast<PFN_vkVoidFunction>(o_vkGetDeviceProcAddr)) == vulkan1 &&
                             ModuleBaseName(vulkan1) == L"vulkan-1.dll" &&
                             !IsStreamlineWsi(reinterpret_cast<PFN_vkVoidFunction>(o_vkGetDeviceProcAddr));
    PVOID resolverTarget = nullptr;
    loaderResolverBound = false;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateWin32SurfaceKHR");
    o_vkCreateWin32SurfaceKHR = (PFN_vkCreateWin32SurfaceKHR) address;

    // address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCmdPipelineBarrier");
    // o_vkCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkGetPhysicalDeviceFeatures2");
    o_vkGetPhysicalDeviceFeatures2 = (PFN_vkGetPhysicalDeviceFeatures2) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkCreateSemaphore");
    o_vkCreateSemaphore = (PFN_vkCreateSemaphore) address;

    address = KernelBaseProxy::GetProcAddress_()(vulkan1, "vkSignalSemaphore");
    o_vkSignalSemaphore = (PFN_vkSignalSemaphore) address;

    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_vkCreateDevice != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkGetInstanceProcAddr != nullptr)
        DetourAttach(&(PVOID&) o_vkGetInstanceProcAddr, hkvkGetInstanceProcAddr);

    if (o_vkGetDeviceProcAddr != nullptr)
        DetourAttachEx(&(PVOID&) o_vkGetDeviceProcAddr, hkvkGetDeviceProcAddr, nullptr, &resolverTarget, nullptr);

    if (o_vkCreateInstance != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourAttach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourAttach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    loaderResolverBound = detourResult == NO_ERROR && loaderOwned && resolverTarget &&
                          ModuleFromAddress(reinterpret_cast<PFN_vkVoidFunction>(resolverTarget)) == vulkan1;
    LOG_INFO("Vulkan overlay original loader resolver bound: {}", loaderResolverBound.load());
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to hook Vulkan, error code: {:X}", detourResult);
        o_vkCreateDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
    }
}

void VulkanHooks::Unhook()
{
    loaderResolverBound = false;
    DetourTransactionBegin();
    DetourUpdateThread(GetCurrentThread());

    if (o_QueuePresentKHR != nullptr)
        DetourDetach(&(PVOID&) o_QueuePresentKHR, hkvkQueuePresentKHR);

    if (o_CreateSwapchainKHR != nullptr)
        DetourDetach(&(PVOID&) o_CreateSwapchainKHR, hkvkCreateSwapchainKHR);

    if (o_DestroySwapchainKHR != nullptr)
        DetourDetach(&(PVOID&) o_DestroySwapchainKHR, hkvkDestroySwapchainKHR);

    if (o_vkCreateDevice != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateDevice, hkvkCreateDevice);

    if (o_vkCreateInstance != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateInstance, hkvkCreateInstance);

    if (o_vkGetInstanceProcAddr != nullptr)
        DetourDetach(&(PVOID&) o_vkGetInstanceProcAddr, hkvkGetInstanceProcAddr);

    if (o_vkGetDeviceProcAddr != nullptr)
        DetourDetach(&(PVOID&) o_vkGetDeviceProcAddr, hkvkGetDeviceProcAddr);

    if (o_vkCreateWin32SurfaceKHR != nullptr)
        DetourDetach(&(PVOID&) o_vkCreateWin32SurfaceKHR, hkvkCreateWin32SurfaceKHR);

    // if (o_vkCmdPipelineBarrier != nullptr)
    //     DetourDetach(&(PVOID&) o_vkCmdPipelineBarrier, hkvkCmdPipelineBarrier);

    auto detourResult = DetourTransactionCommit();
    if (detourResult == NO_ERROR)
    {
        std::lock_guard lock(wsiMutex);
        installedWsi = {};
        deviceStates.clear();
    }
    if (detourResult != NO_ERROR)
    {
        LOG_ERROR("Failed to unhook Vulkan, error code: {:X}", detourResult);
    }
    else
    {
        VulkanHooks::ForgetDevice(_device);
        o_QueuePresentKHR = nullptr;
        o_CreateSwapchainKHR = nullptr;
        o_DestroySwapchainKHR = nullptr;
        o_vkCreateDevice = nullptr;
        o_vkCreateInstance = nullptr;
        o_vkGetInstanceProcAddr = nullptr;
        o_vkGetDeviceProcAddr = nullptr;
        o_vkCreateWin32SurfaceKHR = nullptr;
        // o_vkCmdPipelineBarrier = nullptr;
    }
}
