#pragma once
#include "SysUtils.h"
#include <vulkan/vulkan.h>

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

    static PFN_vkCreateSemaphore o_vkCreateSemaphore;
    static PFN_vkSignalSemaphore o_vkSignalSemaphore;
    static PFN_vkAntiLagUpdateAMD o_vkAntiLagUpdateAMD;

    static void RecordDevice(VkDevice device, VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo* createInfo);
    static bool GetGraphicsQueue(VkDevice device, QueueInfo& info);
    static bool GetPresentQueue(VkDevice device, VkQueue queue, VkSwapchainKHR swapchain, QueueInfo& info,
                                bool* verifiedWsi = nullptr);
    static PFN_vkGetSwapchainImagesKHR GetSwapchainImages(VkDevice device, VkSwapchainKHR swapchain,
                                                          bool* verifiedWsi = nullptr);
    static bool IsVerifiedSwapchain(VkDevice device, VkSwapchainKHR swapchain);
    static void RecordSwapchain(VkDevice device, VkSwapchainKHR swapchain, VkSurfaceKHR surface);
    static void ForgetSwapchain(VkDevice device, VkSwapchainKHR swapchain);
    static void ForgetDevice(VkDevice device);

    static void Hook(HMODULE vulkan1);
    static void Unhook();
};
