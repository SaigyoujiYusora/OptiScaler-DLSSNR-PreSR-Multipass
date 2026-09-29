// MenuOverlayVk::QueuePresent is extracted from production by run_vulkan_overlay.ps1.
// Vulkan entry points and UI services below are counted fakes; no ICD or window is used.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#define LOG_TRACE(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
#define LOG_FUNC() ((void) 0)

struct ImGuiIO
{
    int BackendFlags = 0;
    struct
    {
        float x = 1280.0f;
        float y = 720.0f;
    } DisplaySize;
};
constexpr int ImGuiBackendFlags_RendererHasTextures = 1 << 0;
namespace ImGui
{
ImGuiIO io;
ImGuiIO& GetIO() { return io; }
void* GetDrawData() { return nullptr; }
void Render() {}
} // namespace ImGui

struct ImGui_ImplVulkan_InitInfo
{
    VkDevice Device = VK_NULL_HANDLE;
    VkQueue Queue = VK_NULL_HANDLE;
};
struct ImGui_ImplVulkanH_Frame
{
    VkFence Fence = VK_NULL_HANDLE;
    VkCommandPool CommandPool = VK_NULL_HANDLE;
    VkCommandBuffer CommandBuffer = VK_NULL_HANDLE;
    VkFramebuffer Framebuffer = VK_NULL_HANDLE;
};

void ImGui_ImplVulkan_NewFrame() {}
VkQueue lastPresentQueue = VK_NULL_HANDLE;
uint32_t lastPresentFamily = UINT32_MAX;
uint32_t lastRenderFrame = ~0u;
bool ImGui_ImplVulkan_SetPresentQueue(VkQueue queue, uint32_t family)
{
    lastPresentQueue = queue;
    lastPresentFamily = family;
    return queue != VK_NULL_HANDLE && family == 0;
}
void ImGui_ImplVulkan_RenderDrawData(void*, VkCommandBuffer, VkPipeline = VK_NULL_HANDLE, uint32_t frame = ~0u)
{
    lastRenderFrame = frame;
}

struct FakeState
{
    uint32_t delayMenuRenderBy = 0;
    static FakeState& Instance()
    {
        static FakeState state;
        return state;
    }
};
using State = FakeState;

class MenuOverlayBase
{
  public:
    static bool inited;
    static bool visible;
    static bool IsInited() { return inited; }
    static bool IsVisible() { return visible; }
    static bool renderRequested;
    static bool RenderMenu() { return renderRequested; }
};
bool MenuOverlayBase::inited = true;
bool MenuOverlayBase::visible = true;
bool MenuOverlayBase::renderRequested = true;

class StreamlineHooks
{
  public:
    static bool SyncNativeVulkanDlssgMenu(bool, bool = false) { return allowOverlay; }
    static bool IsNativeVulkanDlssg() { return nativeRoute; }
    static inline bool nativeRoute = true;
    static inline bool allowOverlay = true;
};

class VulkanHooks
{
  public:
    struct QueueInfo
    {
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t familyIndex = UINT32_MAX;
        uint32_t queueIndex = UINT32_MAX;
        VkQueueFlags flags = 0;
        bool protectedQueue = false;
    };
    static bool GetPresentQueue(VkDevice, VkQueue, VkSwapchainKHR, QueueInfo& info, bool* verified)
    {
        if (verified)
            *verified = presentVerified;
        if (!presentAccepted)
            return false;
        info.queue = expectedQueue;
        info.familyIndex = 0;
        info.flags = VK_QUEUE_GRAPHICS_BIT;
        return true;
    }
    static bool IsVerifiedSwapchain(VkDevice, VkSwapchainKHR) { return presentVerified; }
    static inline bool presentAccepted = true;
    static inline bool presentVerified = true;
    static inline VkQueue expectedQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x200));
};

namespace Fake
{
VkResult submitResult = VK_SUCCESS;
unsigned waitFenceCalls = 0;
unsigned resetFenceCalls = 0;
unsigned submitCalls = 0;
uint32_t submitWaitCount = 0;
const VkSemaphore* submitWaits = nullptr;
const VkPipelineStageFlags* submitStages = nullptr;
std::vector<VkSemaphore> copiedWaits;
std::vector<VkPipelineStageFlags> copiedStages;
VkQueue submittedQueue = VK_NULL_HANDLE;
VkFence submittedFence = VK_NULL_HANDLE;
VkCommandBuffer submittedCommandBuffer = VK_NULL_HANDLE;

void Reset()
{
    submitResult = VK_SUCCESS;
    waitFenceCalls = resetFenceCalls = submitCalls = 0;
    submitWaitCount = 0;
    submitWaits = nullptr;
    submitStages = nullptr;
    copiedWaits.clear();
    copiedStages.clear();
    submittedQueue = VK_NULL_HANDLE;
    submittedFence = VK_NULL_HANDLE;
    submittedCommandBuffer = VK_NULL_HANDLE;
}
} // namespace Fake

extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
{
    ++Fake::waitFenceCalls;
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence*)
{
    ++Fake::resetFenceCalls;
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags)
{
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*)
{
    return VK_SUCCESS;
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*,
                                                           VkSubpassContents)
{
}
extern "C" VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer) {}
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
extern "C" VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t, const VkSubmitInfo* submit,
                                                        VkFence fence)
{
    ++Fake::submitCalls;
    Fake::submitWaitCount = submit->waitSemaphoreCount;
    if (submit->waitSemaphoreCount)
    {
        Fake::copiedWaits.assign(submit->pWaitSemaphores, submit->pWaitSemaphores + submit->waitSemaphoreCount);
        Fake::copiedStages.assign(submit->pWaitDstStageMask, submit->pWaitDstStageMask + submit->waitSemaphoreCount);
    }
    else
    {
        Fake::copiedWaits.clear();
        Fake::copiedStages.clear();
    }
    Fake::submitWaits = Fake::copiedWaits.data();
    Fake::submitStages = Fake::copiedStages.data();
    Fake::submittedQueue = queue;
    Fake::submittedFence = fence;
    Fake::submittedCommandBuffer = submit->commandBufferCount ? submit->pCommandBuffers[0] : VK_NULL_HANDLE;
    return Fake::submitResult;
}

bool _vulkanObjectsCreated = true;
std::recursive_mutex _vkPresentMutex;
ImGui_ImplVulkan_InitInfo _ImVulkan_Info { reinterpret_cast<VkDevice>(static_cast<uintptr_t>(0x100)),
                                           reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x200)) };
ImGui_ImplVulkanH_Frame* _ImVulkan_Frames = nullptr;
VkSemaphore* _ImVulkan_Semaphores = nullptr;
VkRenderPass _vkRenderPass = reinterpret_cast<VkRenderPass>(static_cast<uintptr_t>(0x300));
uint32_t _scImageCount = 4;
uint32_t _overlayQueueFamily = 0;
VkSwapchainKHR _overlaySwapchain = reinterpret_cast<VkSwapchainKHR>(static_cast<uintptr_t>(0x400));
VkDevice _overlayDevice = _ImVulkan_Info.Device;
bool _overlayFaulted = false;
uint64_t _frameCount = 0;

namespace MenuOverlayVk
{
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
}

namespace MenuOverlayVk
{
#include "native_vulkan_overlay_production.inc"
} // namespace MenuOverlayVk

[[noreturn]] static void Fail(const char* message)
{
    std::fprintf(stderr, "FAIL: %s\n", message);
    std::exit(1);
}
static void Expect(bool value, const char* message)
{
    if (!value)
        Fail(message);
}

int main()
{
    const auto actualQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x250));
    VulkanHooks::expectedQueue = actualQueue;
    std::array<ImGui_ImplVulkanH_Frame, 4> frames {};
    std::array<VkSemaphore, 4> semaphores {};
    for (unsigned i = 0; i < frames.size(); ++i)
    {
        frames[i].Fence = reinterpret_cast<VkFence>(static_cast<uintptr_t>(0x500 + i));
        frames[i].CommandPool = reinterpret_cast<VkCommandPool>(static_cast<uintptr_t>(0x600 + i));
        frames[i].CommandBuffer = reinterpret_cast<VkCommandBuffer>(static_cast<uintptr_t>(0x700 + i));
        frames[i].Framebuffer = reinterpret_cast<VkFramebuffer>(static_cast<uintptr_t>(0x800 + i));
        semaphores[i] = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0x900 + i));
    }
    _ImVulkan_Frames = frames.data();
    _ImVulkan_Semaphores = semaphores.data();

    std::array<VkSemaphore, 12> waits {};
    for (unsigned i = 0; i < waits.size(); ++i)
        waits[i] = reinterpret_cast<VkSemaphore>(static_cast<uintptr_t>(0xA00 + i));
    uint32_t imageIndex = 2;
    VkPresentInfoKHR chain { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
    chain.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    chain.pWaitSemaphores = waits.data();
    chain.swapchainCount = 1;
    chain.pSwapchains = &_overlaySwapchain;
    chain.pImageIndices = &imageIndex;
    VkResult resultSlot = VK_NOT_READY;
    chain.pResults = &resultSlot;
    void* pNextSentinel = reinterpret_cast<void*>(static_cast<uintptr_t>(0xB00));
    chain.pNext = pNextSentinel;

    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "12-wait overlay present failed");
    Expect(Fake::submitCalls == 1 && Fake::submitWaitCount == waits.size(), "wait semaphore count was truncated");
    for (unsigned i = 0; i < Fake::submitWaitCount; ++i)
        Expect(Fake::copiedStages[i] == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, "wait stage was not ALL_COMMANDS");
    Expect(chain.pNext == pNextSentinel && chain.pResults == &resultSlot, "present pNext/pResults changed");
    Expect(chain.waitSemaphoreCount == 1 && chain.pWaitSemaphores == &semaphores[2],
           "overlay signal semaphore was not installed for the image index");
    Expect(Fake::submittedCommandBuffer == frames[2].CommandBuffer,
           "present used a rotating frame instead of image index");
    Expect(Fake::submittedQueue == actualQueue && lastPresentQueue == actualQueue && lastPresentFamily == 0 &&
               lastRenderFrame == 2,
           "present queue or ImGui frame index was not bound to the real image path");

    // Non-rotating image sequence must select matching frame/semaphore slots.
    for (uint32_t index : { 0u, 3u, 1u })
    {
        imageIndex = index;
        chain.waitSemaphoreCount = 0;
        chain.pWaitSemaphores = nullptr;
        Fake::Reset();
        Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "non-rotating image present failed");
        Expect(Fake::submittedCommandBuffer == frames[index].CommandBuffer, "wrong image slot command buffer");
        Expect(chain.pWaitSemaphores == &semaphores[index], "wrong image slot signal semaphore");
    }

    // FPS/toast frames may request ImGui work while the settings menu is hidden.
    // At the verified native WSI boundary that passive work is allowed even while
    // the normal menu delay countdown is active.
    MenuOverlayBase::visible = false;
    MenuOverlayBase::renderRequested = true;
    State::Instance().delayMenuRenderBy = 10;
    VulkanHooks::presentVerified = true;
    StreamlineHooks::allowOverlay = true;
    imageIndex = 0;
    chain.waitSemaphoreCount = 0;
    chain.pWaitSemaphores = nullptr;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "verified passive overlay present failed");
    Expect(Fake::submitCalls == 1 && Fake::submittedCommandBuffer == frames[0].CommandBuffer,
           "verified passive overlay did not submit using the present image");

    // Late injection may miss slInit, leaving the route flag false even though the
    // independently verified WSI boundary is valid. Legacy SetOptions keeps resetting
    // the menu delay while FG is on; passive overlays must not starve indefinitely.
    StreamlineHooks::nativeRoute = false;
    for (unsigned frame = 0; frame < 120; ++frame)
    {
        State::Instance().delayMenuRenderBy = 10;
        chain.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
        chain.pWaitSemaphores = waits.data();
        Fake::Reset();
        Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "late-injection passive present failed");
        Expect(Fake::submitCalls == 1 && Fake::submittedQueue == actualQueue,
               "missed slInit starved a verified passive overlay behind the legacy menu delay");
    }

    VulkanHooks::presentVerified = false;
    State::Instance().delayMenuRenderBy = 10;
    chain.waitSemaphoreCount = 0;
    chain.pWaitSemaphores = nullptr;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "unverified passive overlay broke application present");
    Expect(Fake::submitCalls == 0, "unverified passive overlay submitted GPU work");

    VulkanHooks::presentVerified = true;
    MenuOverlayBase::visible = true;
    State::Instance().delayMenuRenderBy = 10;
    chain.waitSemaphoreCount = 0;
    chain.pWaitSemaphores = nullptr;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "visible menu delay broke application present");
    Expect(Fake::submitCalls == 0, "visible menu delay submitted GPU work");
    StreamlineHooks::nativeRoute = true;

    // Invalid image/swapchain/queue must return without recording or submitting.
    imageIndex = 9;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "invalid image index broke the application present path");
    Expect(Fake::submitCalls == 0, "invalid image index submitted GPU work");
    imageIndex = 0;
    VkSwapchainKHR wrongSwapchain = reinterpret_cast<VkSwapchainKHR>(static_cast<uintptr_t>(0xDEAD));
    chain.pSwapchains = &wrongSwapchain;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "wrong swapchain broke the application present path");
    Expect(Fake::submitCalls == 0, "wrong swapchain submitted GPU work");
    chain.pSwapchains = &_overlaySwapchain;
    VulkanHooks::presentAccepted = false;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "unsupported queue broke the application present path");
    Expect(Fake::submitCalls == 0, "unsupported queue submitted GPU work");
    VulkanHooks::presentAccepted = true;

    // Compatibility/non-native path: when the overlay policy declines drawing,
    // the application's present remains untouched.
    StreamlineHooks::allowOverlay = false;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "non-native overlay gate broke application present");
    Expect(Fake::submitCalls == 0, "non-native overlay gate submitted GPU work");
    StreamlineHooks::allowOverlay = true;

    // A failed submit must not leave the next frame waiting on an unsignaled fence.
    State::Instance().delayMenuRenderBy = 0;
    MenuOverlayBase::visible = true;
    MenuOverlayBase::renderRequested = true;
    imageIndex = 0;
    chain.pSwapchains = &_overlaySwapchain;
    chain.waitSemaphoreCount = static_cast<uint32_t>(waits.size());
    chain.pWaitSemaphores = waits.data();
    const auto originalWaitCount = chain.waitSemaphoreCount;
    const auto originalWaitPointer = chain.pWaitSemaphores;
    Fake::submitResult = VK_ERROR_DEVICE_LOST;
    Fake::Reset();
    Fake::submitResult = VK_ERROR_DEVICE_LOST;
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "failed submit broke the application present path");
    const auto waitsAfterFailure = Fake::waitFenceCalls;
    Expect(chain.waitSemaphoreCount == originalWaitCount && chain.pWaitSemaphores == originalWaitPointer,
           "failed submit rewrote the application's present wait list");
    Fake::submitResult = VK_SUCCESS;
    Fake::Reset();
    Expect(MenuOverlayVk::QueuePresent(actualQueue, &chain), "recovery after failed submit failed");
    Expect(Fake::waitFenceCalls == 0 && waitsAfterFailure == 1,
           "recovery waited on a fence made unsignaled by failed submit");

    std::puts("PASS: Vulkan overlay QueuePresent wait preservation, image-index mapping, WSI guards, pNext/pResults, "
              "and submit failure recovery");
    return 0;
}
