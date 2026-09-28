#include "pch.h"
#include "DlssNr_CompatibilityRuntime.h"
#include "DlssNr_RuntimeImports.h"
#include <Logger.h>
#include <d3d12.h>
#include <nvsdk_ngx.h>
#include <psapi.h>
#include <algorithm>
#include <mutex>
#include <map>
#include <string>
#include <vector>
#include <set>
#include <condition_variable>
#include <atomic>
#include <functional>
#pragma comment(lib, "psapi.lib")

namespace DlssNr
{
namespace
{
std::recursive_mutex registryMutex;
std::condition_variable_any moduleRetired;
bool moduleRegistered = false;
// Only the model's import is redirected. Outside a direct NR call, even that import
// reports the real path. No process-wide hook, driver patch, or on-disk change.
thread_local HMODULE callerAlias = nullptr;
// Preserve a loader/overlay's existing IAT wrapper for ordinary path queries.
std::atomic<decltype(&GetModuleFileNameW)> originalPathW { &GetModuleFileNameW };
std::atomic<decltype(&GetModuleFileNameA)> originalPathA { &GetModuleFileNameA };
DWORD WINAPI CallerPath(HMODULE queried, LPWSTR path, DWORD capacity)
{
    const auto original = originalPathW.load();
    if (callerAlias && queried == callerAlias)
    {
        constexpr wchar_t alias[] = L"nvngx.dll";
        if (!capacity)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        const DWORD copied = (DWORD) std::min<size_t>(capacity - 1, std::size(alias) - 1);
        memcpy(path, alias, copied * sizeof(wchar_t));
        path[copied] = 0;
        if (capacity < std::size(alias))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return capacity;
        }
        return copied;
    }
    return original(queried, path, capacity);
}

DWORD WINAPI CallerPathA(HMODULE queried, LPSTR path, DWORD capacity)
{
    const auto original = originalPathA.load();
    if (callerAlias && queried == callerAlias)
    {
        constexpr char alias[] = "nvngx.dll";
        if (!capacity)
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return 0;
        }
        const DWORD copied = (DWORD) std::min<size_t>(capacity - 1, std::size(alias) - 1);
        memcpy(path, alias, copied);
        path[copied] = 0;
        if (capacity < std::size(alias))
        {
            SetLastError(ERROR_INSUFFICIENT_BUFFER);
            return capacity;
        }
        return copied;
    }
    return original(queried, path, capacity);
}

struct CallerScope
{
    HMODULE previous = callerAlias;
    CallerScope()
    {
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&CallerPath), &callerAlias);
    }
    ~CallerScope() { callerAlias = previous; }
};

bool ReplaceImport(void** slot, void* expected, void* replacement)
{
    if (*slot != expected)
    {
        LOG_ERROR("NR compatibility: import changed before replacement: slot={} expected={} actual={}", (void*) slot,
                  expected, *slot);
        return false;
    }
    DWORD protection = 0;
    if (!VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &protection))
    {
        LOG_ERROR("NR compatibility: import protection change failed: slot={} error={}", (void*) slot, GetLastError());
        return false;
    }
    const bool replaced = InterlockedCompareExchangePointer(slot, replacement, expected) == expected;
    DWORD ignored = 0;
    if (!VirtualProtect(slot, sizeof(void*), protection, &ignored))
        LOG_WARN("NR compatibility: import protection restore failed: slot={} error={}", (void*) slot, GetLastError());
    if (!replaced)
        LOG_ERROR("NR compatibility: import changed concurrently: slot={}", (void*) slot);
    return replaced;
}

enum class RuntimeApi
{
    Dx12,
    Vulkan,
};

struct RuntimeKey
{
    RuntimeApi api;
    void* device;

    bool operator<(const RuntimeKey& other) const noexcept
    {
        if (api != other.api)
            return api < other.api;
        return std::less<void*> {}(device, other.device);
    }
};

struct RuntimeRegistry
{
    std::map<RuntimeKey, std::weak_ptr<CompatibilityRuntime>> owners;
};

RuntimeRegistry& Registry()
{
    static RuntimeRegistry registry;
    return registry;
}

void PruneOwners()
{
    auto& owners = Registry().owners;
    for (auto it = owners.begin(); it != owners.end();)
    {
        if (it->second.expired())
            it = owners.erase(it);
        else
            ++it;
    }
}

RuntimeKey Key(RuntimeApi api, ID3D12Device* device) { return { api, device }; }

RuntimeKey Key(RuntimeApi api, VkDevice device) { return { api, reinterpret_cast<void*>(device) }; }
} // namespace

struct CompatibilityRuntime::Module
{
    struct Binding
    {
        RuntimeImports::Slot slot;
        void* original;
    };
    HMODULE handle = nullptr;
    bool borrowed = false;
    bool registered = false;
    std::vector<Binding> imports;
    std::filesystem::path path;
    std::recursive_mutex mutex;
    std::set<RuntimeKey> initializedDevices;
    std::condition_variable_any deviceRetired;
    // This weak registration remains valid while any API/device owner keeps the module alive.
    // It avoids using the most recently created owner as a proxy for module lifetime.
    inline static std::weak_ptr<Module> liveModule;
    // The compatibility DLL uses the private NR Init_Ext ABI. It accepts the driver-owned
    // capability block for D3D12; Vulkan's verified forwarder ABI instead ends in a null
    // feature-info pointer followed by SDK version 0x15.
    using Dx12Init = NVSDK_NGX_Result (*)(unsigned long long, const wchar_t*, ID3D12Device*, unsigned int,
                                          NVSDK_NGX_Parameter*);
    using VulkanInit = NVSDK_NGX_Result(NVSDK_CONV*)(unsigned long long, const wchar_t*, VkInstance, VkPhysicalDevice,
                                                     VkDevice, const void*, unsigned int);
    Dx12Init dx12Init = nullptr;
    decltype(&NVSDK_NGX_D3D12_CreateFeature) dx12Create = nullptr;
    decltype(&NVSDK_NGX_D3D12_EvaluateFeature) dx12Evaluate = nullptr;
    decltype(&NVSDK_NGX_D3D12_ReleaseFeature) dx12Release = nullptr;
    using Dx12Shutdown = NVSDK_NGX_Result (*)(ID3D12Device*);
    Dx12Shutdown dx12Shutdown = nullptr;
    VulkanInit vulkanInit = nullptr;
    decltype(&NVSDK_NGX_VULKAN_CreateFeature) vulkanCreate = nullptr;
    decltype(&NVSDK_NGX_VULKAN_EvaluateFeature) vulkanEvaluate = nullptr;
    decltype(&NVSDK_NGX_VULKAN_ReleaseFeature) vulkanRelease = nullptr;
    decltype(&NVSDK_NGX_VULKAN_Shutdown1) vulkanShutdown = nullptr;

    ~Module()
    {
        std::lock_guard registryLock(registryMutex);
        for (const auto& binding : imports)
            ReplaceImport(binding.slot.address,
                          binding.slot.wide ? reinterpret_cast<void*>(&CallerPath)
                                            : reinterpret_cast<void*>(&CallerPathA),
                          binding.original);
        if (handle)
            FreeLibrary(handle);
        if (registered)
        {
            liveModule.reset();
            moduleRegistered = false;
            moduleRetired.notify_all();
        }
    }
};

std::shared_ptr<CompatibilityRuntime::Module> CompatibilityRuntime::LoadModule(const std::filesystem::path& candidate)
{
    if (auto loaded = Module::liveModule.lock())
    {
        if (loaded->path != candidate)
        {
            std::error_code error;
            if (!std::filesystem::equivalent(loaded->path, candidate, error) || error)
                return {};
        }
        return loaded;
    }

    auto loaded = std::make_shared<Module>();
    loaded->path = candidate;
    // Acquire our own reference to this exact module if NGX/another loader already loaded it.
    // Its original owner keeps responsibility for device-wide shutdown.
    loaded->borrowed = GetModuleHandleExW(0, candidate.c_str(), &loaded->handle) != FALSE;
    if (!loaded->handle)
        loaded->handle = LoadLibraryExW(candidate.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!loaded->handle)
    {
        const auto error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND && error != ERROR_MOD_NOT_FOUND)
            LOG_ERROR("NR compatibility: cannot load {} ({})", candidate.string(), error);
        return {};
    }

    auto symbol = [&](const char* name) { return GetProcAddress(loaded->handle, name); };
    loaded->dx12Init = reinterpret_cast<Module::Dx12Init>(symbol("NVSDK_NGX_D3D12_Init_Ext"));
    loaded->dx12Create = reinterpret_cast<decltype(loaded->dx12Create)>(symbol("NVSDK_NGX_D3D12_CreateFeature"));
    loaded->dx12Evaluate = reinterpret_cast<decltype(loaded->dx12Evaluate)>(symbol("NVSDK_NGX_D3D12_EvaluateFeature"));
    loaded->dx12Release = reinterpret_cast<decltype(loaded->dx12Release)>(symbol("NVSDK_NGX_D3D12_ReleaseFeature"));
    loaded->dx12Shutdown = reinterpret_cast<Module::Dx12Shutdown>(symbol("NVSDK_NGX_D3D12_Shutdown1"));
    loaded->vulkanInit = reinterpret_cast<Module::VulkanInit>(symbol("NVSDK_NGX_VULKAN_Init_Ext"));
    loaded->vulkanCreate = reinterpret_cast<decltype(loaded->vulkanCreate)>(symbol("NVSDK_NGX_VULKAN_CreateFeature"));
    loaded->vulkanEvaluate =
        reinterpret_cast<decltype(loaded->vulkanEvaluate)>(symbol("NVSDK_NGX_VULKAN_EvaluateFeature"));
    loaded->vulkanRelease =
        reinterpret_cast<decltype(loaded->vulkanRelease)>(symbol("NVSDK_NGX_VULKAN_ReleaseFeature"));
    loaded->vulkanShutdown = reinterpret_cast<decltype(loaded->vulkanShutdown)>(symbol("NVSDK_NGX_VULKAN_Shutdown1"));
    const bool hasDx12 =
        loaded->dx12Init && loaded->dx12Create && loaded->dx12Evaluate && loaded->dx12Release && loaded->dx12Shutdown;
    const bool hasVulkan = loaded->vulkanInit && loaded->vulkanCreate && loaded->vulkanEvaluate &&
                           loaded->vulkanRelease && loaded->vulkanShutdown;
    if (!hasDx12 && !hasVulkan)
    {
        LOG_ERROR("NR compatibility: {} is missing required NR exports", candidate.string());
        return {};
    }

    MODULEINFO image {};
    std::vector<RuntimeImports::Slot> slots;
    if (!GetModuleInformation(GetCurrentProcess(), loaded->handle, &image, sizeof(image)) ||
        !RuntimeImports::Find({ static_cast<unsigned char*>(image.lpBaseOfDll), image.SizeOfImage }, slots))
    {
        LOG_ERROR("NR compatibility: invalid runtime import table in {}", candidate.string());
        return {};
    }
    // Resolve named imports from this build; never use a version-specific address. This is performed
    // once for the shared Module so opening the second API cannot overwrite the first API's hook.
    loaded->imports.reserve(slots.size());
    void* previousW = nullptr;
    void* previousA = nullptr;
    for (const auto& slot : slots)
    {
        auto*& previous = slot.wide ? previousW : previousA;
        if (!*slot.address || (previous && previous != *slot.address))
        {
            LOG_ERROR("NR compatibility: conflicting caller-path import targets in {}", candidate.string());
            return {};
        }
        previous = *slot.address;
    }
    if (previousW)
        originalPathW = reinterpret_cast<decltype(&GetModuleFileNameW)>(previousW);
    if (previousA)
        originalPathA = reinterpret_cast<decltype(&GetModuleFileNameA)>(previousA);
    for (const auto& slot : slots)
    {
        auto* previous = slot.wide ? previousW : previousA;
        if (!ReplaceImport(slot.address, previous,
                           slot.wide ? reinterpret_cast<void*>(&CallerPath) : reinterpret_cast<void*>(&CallerPathA)))
        {
            LOG_ERROR("NR compatibility: cannot adapt caller-path import in {}", candidate.string());
            return {};
        }
        loaded->imports.push_back({ slot, previous });
    }
    loaded->registered = moduleRegistered = true;
    Module::liveModule = loaded;
    moduleRetired.notify_all();
    LOG_INFO("NR compatibility: {} {} with {} named caller-path imports, no helper DLL",
             loaded->borrowed ? "retained existing runtime" : "loaded", candidate.string(), slots.size());
    return loaded;
}

std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::Open(const std::filesystem::path& candidate,
                                                                 ID3D12Device* device, Allocate allocate,
                                                                 Destroy destroy, const std::filesystem::path& dataPath)
{
    if (!device || !allocate || !destroy)
        return {};
    std::unique_lock lock(registryMutex);
    try
    {
        const auto path = std::filesystem::absolute(candidate).lexically_normal();
        auto& registry = Registry();
        auto liveModule = Module::liveModule.lock();
        if (!liveModule)
            moduleRetired.wait(lock,
                               [&]
                               {
                                   liveModule = Module::liveModule.lock();
                                   return liveModule || !moduleRegistered;
                               });
        PruneOwners();
        auto loaded = LoadModule(path);
        if (!loaded)
            return {};
        const auto key = Key(RuntimeApi::Dx12, device);
        if (auto it = registry.owners.find(key); it != registry.owners.end())
            if (auto existing = it->second.lock())
                return existing;

        if (!loaded->dx12Init || !loaded->dx12Create || !loaded->dx12Evaluate || !loaded->dx12Release ||
            !loaded->dx12Shutdown)
        {
            LOG_ERROR("NR compatibility: {} is missing required D3D12 NR exports", path.string());
            return {};
        }

        auto owner = std::shared_ptr<CompatibilityRuntime>(new CompatibilityRuntime());
        owner->module = loaded;
        owner->device = device;
        owner->destroyParameters = destroy;
        device->AddRef();
        std::unique_lock runtimeLock(loaded->mutex);
        // An expired weak owner may still be executing its destructor on another thread.
        loaded->deviceRetired.wait(runtimeLock, [&] { return !loaded->initializedDevices.contains(key); });
        auto result = allocate(&owner->capabilities);
        if (result != NVSDK_NGX_Result_Success || !owner->capabilities)
            return {};
        CallerScope caller;
        const auto writablePath = dataPath.empty() ? std::filesystem::temp_directory_path() : dataPath;
        result = loaded->dx12Init(0x24480451ull, writablePath.c_str(), device, 0x15, owner->capabilities);
        LOG_INFO("NR compatibility: D3D12 Init_Ext result=0x{:08X}", (unsigned) result);
        if (result != NVSDK_NGX_Result_Success)
            return {};
        owner->initialized = true;
        loaded->initializedDevices.insert(key);
        registry.owners[key] = owner;
        return owner;
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("NR compatibility: {}", error.what());
        return {};
    }
}

std::shared_ptr<CompatibilityRuntime> CompatibilityRuntime::OpenVulkan(const std::filesystem::path& candidate,
                                                                       VkInstance instance,
                                                                       VkPhysicalDevice physicalDevice, VkDevice device,
                                                                       Allocate allocate, Destroy destroy,
                                                                       const std::filesystem::path& dataPath)
{
    if (instance == VK_NULL_HANDLE || physicalDevice == VK_NULL_HANDLE || device == VK_NULL_HANDLE || !allocate ||
        !destroy)
        return {};
    std::unique_lock lock(registryMutex);
    try
    {
        const auto path = std::filesystem::absolute(candidate).lexically_normal();
        auto& registry = Registry();
        auto liveModule = Module::liveModule.lock();
        if (!liveModule)
            moduleRetired.wait(lock,
                               [&]
                               {
                                   liveModule = Module::liveModule.lock();
                                   return liveModule || !moduleRegistered;
                               });
        PruneOwners();
        auto loaded = LoadModule(path);
        if (!loaded)
            return {};
        const auto key = Key(RuntimeApi::Vulkan, device);
        if (auto it = registry.owners.find(key); it != registry.owners.end())
            if (auto existing = it->second.lock())
            {
                if (existing->instance != instance || existing->physicalDevice != physicalDevice)
                {
                    LOG_ERROR(
                        "NR compatibility: Vulkan device {} was opened with a different instance or physical device",
                        (void*) device);
                    return {};
                }
                return existing;
            }

        if (!loaded->vulkanInit || !loaded->vulkanCreate || !loaded->vulkanEvaluate || !loaded->vulkanRelease ||
            !loaded->vulkanShutdown)
        {
            LOG_ERROR("NR compatibility: {} is missing required Vulkan NR exports", path.string());
            return {};
        }

        auto owner = std::shared_ptr<CompatibilityRuntime>(new CompatibilityRuntime());
        owner->module = loaded;
        owner->instance = instance;
        owner->physicalDevice = physicalDevice;
        owner->vkDevice = device;
        owner->destroyParameters = destroy;
        owner->vulkan = true;
        std::unique_lock runtimeLock(loaded->mutex);
        // The upper Vulkan feature owns queue/device retirement. This lock only serializes NGX's
        // process-global compatibility module and prevents two owners initializing one device.
        loaded->deviceRetired.wait(runtimeLock, [&] { return !loaded->initializedDevices.contains(key); });
        auto result = allocate(&owner->capabilities);
        if (result != NVSDK_NGX_Result_Success || !owner->capabilities)
            return {};
        CallerScope caller;
        const auto writablePath = dataPath.empty() ? std::filesystem::temp_directory_path() : dataPath;
        // This is the ABI used by the verified Dagherbou forwarder: null feature-info pointer, then 0x15.
        result = loaded->vulkanInit(0ull, writablePath.c_str(), instance, physicalDevice, device, nullptr, 0x15);
        LOG_INFO("NR compatibility: Vulkan Init_Ext result=0x{:08X}", (unsigned) result);
        if (result != NVSDK_NGX_Result_Success)
            return {};
        owner->initialized = true;
        loaded->initializedDevices.insert(key);
        registry.owners[key] = owner;
        return owner;
    }
    catch (const std::exception& error)
    {
        LOG_ERROR("NR compatibility: {}", error.what());
        return {};
    }
}

CompatibilityRuntime::~CompatibilityRuntime()
{
    if (!module)
        return;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    const auto key = vulkan ? Key(RuntimeApi::Vulkan, vkDevice) : Key(RuntimeApi::Dx12, device);
    if (initialized)
    {
        if (!module->borrowed)
        {
            if (vulkan && module->vulkanShutdown)
            {
                const auto result = module->vulkanShutdown(vkDevice);
                LOG_INFO("NR compatibility: Vulkan Shutdown1 result=0x{:08X}", (unsigned) result);
            }
            else if (!vulkan && module->dx12Shutdown)
            {
                const auto result = module->dx12Shutdown(device);
                LOG_INFO("NR compatibility: D3D12 Shutdown1 result=0x{:08X}", (unsigned) result);
            }
        }
        module->initializedDevices.erase(key);
        module->deviceRetired.notify_all();
    }
    if (capabilities && destroyParameters)
        destroyParameters(capabilities);
    if (device)
        device->Release();
}

NVSDK_NGX_Result CompatibilityRuntime::Create(ID3D12GraphicsCommandList* commands, NVSDK_NGX_Parameter* params,
                                              NVSDK_NGX_Handle** feature)
{
    if (!module || vulkan || !module->dx12Create)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    return module->dx12Create(commands, (NVSDK_NGX_Feature) 18, params, feature);
}

NVSDK_NGX_Result CompatibilityRuntime::Create(VkCommandBuffer commands, NVSDK_NGX_Parameter* params,
                                              NVSDK_NGX_Handle** feature)
{
    if (!module || !vulkan || !module->vulkanCreate)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    return module->vulkanCreate(commands, (NVSDK_NGX_Feature) 18, params, feature);
}

NVSDK_NGX_Result CompatibilityRuntime::Evaluate(ID3D12GraphicsCommandList* commands, const NVSDK_NGX_Handle* feature,
                                                NVSDK_NGX_Parameter* params)
{
    if (!module || vulkan || !module->dx12Evaluate)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    return module->dx12Evaluate(commands, feature, params, nullptr);
}

NVSDK_NGX_Result CompatibilityRuntime::Evaluate(VkCommandBuffer commands, const NVSDK_NGX_Handle* feature,
                                                NVSDK_NGX_Parameter* params)
{
    if (!module || !vulkan || !module->vulkanEvaluate)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    return module->vulkanEvaluate(commands, feature, params, nullptr);
}

NVSDK_NGX_Result CompatibilityRuntime::Release(NVSDK_NGX_Handle* feature)
{
    if (!module)
        return NVSDK_NGX_Result_FAIL_InvalidParameter;
    std::lock_guard lock(module->mutex);
    CallerScope caller;
    if (vulkan && module->vulkanRelease)
        return module->vulkanRelease(feature);
    if (!vulkan && module->dx12Release)
        return module->dx12Release(feature);
    return NVSDK_NGX_Result_FAIL_InvalidParameter;
}
} // namespace DlssNr
