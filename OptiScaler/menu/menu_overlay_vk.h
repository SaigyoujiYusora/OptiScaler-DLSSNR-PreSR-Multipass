#pragma once

#include "SysUtils.h"
#include <vulkan/vulkan.hpp>

namespace MenuOverlayVk
{
class PresentScope
{
  public:
    PresentScope();
    ~PresentScope();
    PresentScope(const PresentScope&) = delete;
    PresentScope& operator=(const PresentScope&) = delete;
};

void CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                     const VkSwapchainCreateInfoKHR* pCreateInfo, const VkAllocationCallbacks* pAllocator,
                     VkSwapchainKHR* pSwapchain);
void DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain);
bool QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo);
void DestroyVulkanObjects(bool shutdown);
} // namespace MenuOverlayVk
