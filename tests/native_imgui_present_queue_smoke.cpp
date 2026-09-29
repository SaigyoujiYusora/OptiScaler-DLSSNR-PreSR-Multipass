// ImGui Vulkan backend function body is extracted by run_vulkan_overlay.ps1.
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>

struct ImGui_ImplVulkan_InitInfo
{
    VkQueue Queue = VK_NULL_HANDLE;
    uint32_t QueueFamily = UINT32_MAX;
};
struct ImGui_ImplVulkan_Data
{
    ImGui_ImplVulkan_InitInfo VulkanInitInfo {};
};
ImGui_ImplVulkan_Data backendData;
ImGui_ImplVulkan_Data* ImGui_ImplVulkan_GetBackendData() { return &backendData; }

#include "imgui_present_queue_production.inc"

static void Expect(bool condition, const char* message)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main()
{
    const auto initializedQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x100));
    const auto presentQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x200));
    const auto otherQueue = reinterpret_cast<VkQueue>(static_cast<uintptr_t>(0x300));
    backendData.VulkanInitInfo.Queue = initializedQueue;
    backendData.VulkanInitInfo.QueueFamily = 7;

    Expect(ImGui_ImplVulkan_SetPresentQueue(presentQueue, 7), "matching present queue/family was rejected");
    Expect(backendData.VulkanInitInfo.Queue == presentQueue, "matching present queue was not installed");
    Expect(!ImGui_ImplVulkan_SetPresentQueue(otherQueue, 8), "wrong queue family was accepted");
    Expect(backendData.VulkanInitInfo.Queue == presentQueue, "wrong family changed backend queue");
    Expect(!ImGui_ImplVulkan_SetPresentQueue(VK_NULL_HANDLE, 7), "null present queue was accepted");
    Expect(backendData.VulkanInitInfo.Queue == presentQueue, "null queue changed backend queue");
    std::puts("PASS: ImGui Vulkan present queue family validation and rebinding");
    return 0;
}
