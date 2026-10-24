#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <hooks/Vulkan_Wsi.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#define LOG_INFO(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_FUNC() ((void)0)
using PVOID = void*;
using LONG = long;
constexpr LONG NO_ERROR = 0, ERROR_INVALID_FUNCTION = 1;
template <typename T> T Handle(uintptr_t value) { return reinterpret_cast<T>(value); }

struct State {
    bool vulkanSkipHooks = false;
    static State& Instance() { static State state; return state; }
};
class VulkanHooks {
public:
    struct QueueInfo {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t familyIndex = UINT32_MAX, queueIndex = UINT32_MAX;
        VkQueueFlags flags = 0;
        bool protectedQueue = false;
    };
    static bool GetGraphicsQueue(VkDevice, QueueInfo&);
    static bool GetDeviceContext(VkDevice,VkPhysicalDevice&,VkInstance&);
    static bool GetPresentQueue(VkDevice,VkQueue,VkSwapchainKHR,QueueInfo&,bool*);
    static PFN_vkGetSwapchainImagesKHR GetSwapchainImages(VkDevice,VkSwapchainKHR,bool*);
    static bool IsVerifiedSwapchain(VkDevice,VkSwapchainKHR);
    static void RecordSwapchain(VkDevice,VkSwapchainKHR,VkSurfaceKHR);
    static bool ConfirmSwapchainImages(VkDevice,VkSwapchainKHR,const VkImage*,uint32_t);
    static void ForgetSwapchain(VkDevice,VkSwapchainKHR);
    static void ForgetDevice(VkDevice);
};
PFN_vkQueuePresentKHR o_QueuePresentKHR = nullptr;
PFN_vkCreateSwapchainKHR o_CreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR o_DestroySwapchainKHR = nullptr;
VkResult VKAPI_CALL hkvkQueuePresentKHR(VkQueue,const VkPresentInfoKHR*) { return VK_SUCCESS; }
VkResult VKAPI_CALL hkvkCreateSwapchainKHR(VkDevice,const VkSwapchainCreateInfoKHR*,const VkAllocationCallbacks*,VkSwapchainKHR*) { return VK_SUCCESS; }
void VKAPI_CALL hkvkDestroySwapchainKHR(VkDevice,VkSwapchainKHR,const VkAllocationCallbacks*) {}
PFN_vkVoidFunction proxyEntry = Handle<PFN_vkVoidFunction>(0x9990);
bool IsStreamlineWsi(PFN_vkVoidFunction entry) { return !entry || entry == proxyEntry; }
LONG attachResult = 0, commitResult = 0, beginResult = 0;
unsigned transactions = 0, aborts = 0;
bool retargetProxy = false;
LONG DetourTransactionBegin() { ++transactions; return beginResult; }
LONG DetourUpdateThread(void*) { return 0; }
void* GetCurrentThread() { return nullptr; }
template <typename F> LONG DetourAttachEx(void** entry,F,void*,void** target,void*) {
    *target = retargetProxy ? reinterpret_cast<void*>(proxyEntry) : *entry;
    return attachResult;
}
LONG DetourTransactionAbort() { ++aborts; return 0; }
LONG DetourTransactionCommit() { return commitResult; }

#include "vulkan_wsi_production.inc"

unsigned resolveCalls = 0;
PFN_vkVoidFunction VKAPI_CALL RawResolver(VkDevice device,const char* name) {
    ++resolveCalls;
    uintptr_t offset = device == Handle<VkDevice>(3) ? 0x100 : 0;
    if (std::string_view(name) == "vkCreateSwapchainKHR") return Handle<PFN_vkVoidFunction>(0x1010+offset);
    if (std::string_view(name) == "vkDestroySwapchainKHR") return Handle<PFN_vkVoidFunction>(0x2010+offset);
    if (std::string_view(name) == "vkQueuePresentKHR") return Handle<PFN_vkVoidFunction>(0x3010+offset);
    if (std::string_view(name) == "vkGetSwapchainImagesKHR") return Handle<PFN_vkVoidFunction>(0x4010+offset);
    return nullptr;
}
bool surfaceSupported = true;
VkResult VKAPI_CALL SurfaceSupport(VkPhysicalDevice,uint32_t family,VkSurfaceKHR,VkBool32* result) {
    *result = surfaceSupported && family == 0; return VK_SUCCESS;
}
void AddDevice(uintptr_t id,bool native=true) {
    DeviceState state;
    state.physicalDevice = Handle<VkPhysicalDevice>(id+8);
    state.instance = Handle<VkInstance>(id+16);
    state.dispatch = VulkanWsi::Resolve(RawResolver,Handle<VkDevice>(id));
    state.nativeLoaderChain = native;
    state.getSurfaceSupport = SurfaceSupport;
    auto queue = Handle<VkQueue>(id+0x1000);
    state.queues.emplace(id+0x1000,VulkanHooks::QueueInfo{queue,0,0,VK_QUEUE_GRAPHICS_BIT,false});
    deviceStates[id] = std::move(state);
}
void Reset() {
    deviceStates.clear(); installedWsi={};
    o_QueuePresentKHR=nullptr; o_CreateSwapchainKHR=nullptr; o_DestroySwapchainKHR=nullptr;
    attachResult=commitResult=beginResult=0; transactions=aborts=0; retargetProxy=false;
}
int main() {
    const auto device=Handle<VkDevice>(1);
    const auto dispatch=VulkanWsi::Resolve(RawResolver,device);
    assert(resolveCalls == 4 && dispatch.Complete());
    assert(VulkanWsi::IsNativeChain(dispatch,true,IsStreamlineWsi));
    assert(!VulkanWsi::IsNativeChain(dispatch,false,IsStreamlineWsi));
    assert(!VulkanWsi::Resolve(nullptr,device).Complete());
    auto partial=dispatch; partial.images=nullptr;
    assert(!VulkanWsi::IsNativeChain(partial,true,IsStreamlineWsi));
    auto proxy=dispatch; proxy.images=reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(proxyEntry);
    assert(!VulkanWsi::IsNativeChain(proxy,true,IsStreamlineWsi));
    proxy=dispatch; proxy.present=reinterpret_cast<PFN_vkQueuePresentKHR>(proxyEntry);
    assert(!VulkanWsi::IsNativeChain(proxy,true,IsStreamlineWsi));
    std::puts("PASS: one loader resolves all WSI entries; arbitrary transparent entry identities accepted; proxy/missing provenance rejected");

    for (unsigned failure=0;failure<4;++failure) {
        Reset(); AddDevice(1);
        if(failure==0)beginResult=1;
        if(failure==1)attachResult=1;
        if(failure==2)commitResult=1;
        if(failure==3)retargetProxy=true;
        HookDevice(device);
        assert(!deviceStates[1].verifiedWsi && !installedWsi.Complete());
        assert(!o_QueuePresentKHR && !o_CreateSwapchainKHR && !o_DestroySwapchainKHR);
    }
    Reset(); AddDevice(1); HookDevice(device);
    assert(deviceStates[1].verifiedWsi && transactions==1);
    const auto beforeResolve=resolveCalls;
    AddDevice(2); const auto afterRecord=resolveCalls; HookDevice(Handle<VkDevice>(2));
    assert(deviceStates[2].verifiedWsi && transactions==1 && resolveCalls==afterRecord && afterRecord==beforeResolve+4);
    AddDevice(3); HookDevice(Handle<VkDevice>(3));
    assert(!deviceStates[3].verifiedWsi && transactions==1);
    AddDevice(4,false); HookDevice(Handle<VkDevice>(4));
    assert(!deviceStates[4].verifiedWsi);
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    assert(VulkanHooks::GetDeviceContext(device,physical,instance));
    assert(physical==Handle<VkPhysicalDevice>(9) && instance==Handle<VkInstance>(17));
    assert(!VulkanHooks::GetDeviceContext(Handle<VkDevice>(3),physical,instance));
    assert(!physical && !instance);
    assert(VulkanHooks::GetDeviceContext(device,physical,instance));
    assert(physical==Handle<VkPhysicalDevice>(9) && instance==Handle<VkInstance>(17));
    std::puts("PASS: failed/retargeted hooks never bind; shared targets bind second device; different targets rejected without re-resolving");

    const auto rawSwapchain=Handle<VkSwapchainKHR>(0x5000), proxySwapchain=Handle<VkSwapchainKHR>(0x6000);
    const auto surface=Handle<VkSurfaceKHR>(0x7000);
    const auto queue=Handle<VkQueue>(0x1001);
    const VkImage images[]={Handle<VkImage>(0x8000),Handle<VkImage>(0x8001)};
    VulkanHooks::QueueInfo info; bool verified=false;
    assert(!VulkanHooks::GetPresentQueue(device,queue,proxySwapchain,info,&verified));
    VulkanHooks::RecordSwapchain(device,rawSwapchain,surface);
    assert(!VulkanHooks::IsVerifiedSwapchain(device,rawSwapchain));
    assert(VulkanHooks::GetSwapchainImages(device,rawSwapchain,nullptr)==dispatch.images);
    assert(!VulkanHooks::ConfirmSwapchainImages(device,rawSwapchain,nullptr,2));
    const VkImage duplicate[]={images[0],images[0]};
    assert(!VulkanHooks::ConfirmSwapchainImages(device,rawSwapchain,duplicate,2));
    assert(VulkanHooks::ConfirmSwapchainImages(device,rawSwapchain,images,2));
    assert(VulkanHooks::GetPresentQueue(device,queue,rawSwapchain,info,&verified) && verified);
    assert(!VulkanHooks::GetPresentQueue(Handle<VkDevice>(2),queue,rawSwapchain,info,&verified));
    assert(!VulkanHooks::GetPresentQueue(device,Handle<VkQueue>(0x9999),rawSwapchain,info,&verified));
    assert(!VulkanHooks::GetPresentQueue(device,queue,proxySwapchain,info,&verified));
    surfaceSupported=false;
    assert(!VulkanHooks::GetPresentQueue(device,queue,rawSwapchain,info,&verified));
    surfaceSupported=true;
    VulkanHooks::ForgetSwapchain(device,rawSwapchain);
    assert(!VulkanHooks::IsVerifiedSwapchain(device,rawSwapchain));
    VulkanHooks::RecordSwapchain(Handle<VkDevice>(3),rawSwapchain,surface);
    assert(!VulkanHooks::GetSwapchainImages(Handle<VkDevice>(3),rawSwapchain,nullptr));
    VulkanHooks::ForgetDevice(device);
    assert(!VulkanHooks::GetPresentQueue(device,queue,rawSwapchain,info,&verified));
    std::puts("PASS: only raw-created swapchain plus valid images can present; virtual input/unbound owner/wrong queue/destroyed objects rejected");
}
