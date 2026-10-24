#pragma once
#include <vulkan/vulkan.h>

namespace VulkanWsi
{
// These entries must be resolved together from the original loader for one device.
// Streamline's virtual input swapchain uses a separate proxy resolver.
struct Dispatch
{
    PFN_vkCreateSwapchainKHR create = nullptr;
    PFN_vkDestroySwapchainKHR destroy = nullptr;
    PFN_vkQueuePresentKHR present = nullptr;
    PFN_vkGetSwapchainImagesKHR images = nullptr;

    bool Complete() const { return create && destroy && present && images; }
    bool SameHooks(const Dispatch& other) const
    {
        return Complete() && other.Complete() && create == other.create && destroy == other.destroy &&
               present == other.present;
    }
};

inline Dispatch Resolve(PFN_vkGetDeviceProcAddr resolver, VkDevice device)
{
    if (!resolver || device == VK_NULL_HANDLE)
        return {};
    return { reinterpret_cast<PFN_vkCreateSwapchainKHR>(resolver(device, "vkCreateSwapchainKHR")),
             reinterpret_cast<PFN_vkDestroySwapchainKHR>(resolver(device, "vkDestroySwapchainKHR")),
             reinterpret_cast<PFN_vkQueuePresentKHR>(resolver(device, "vkQueuePresentKHR")),
             reinterpret_cast<PFN_vkGetSwapchainImagesKHR>(resolver(device, "vkGetSwapchainImagesKHR")) };
}

template <typename IsProxy> bool IsNativeChain(const Dispatch& dispatch, bool loaderOwned, IsProxy isProxy)
{
    return loaderOwned && dispatch.Complete() &&
           !isProxy(reinterpret_cast<PFN_vkVoidFunction>(dispatch.create)) &&
           !isProxy(reinterpret_cast<PFN_vkVoidFunction>(dispatch.destroy)) &&
           !isProxy(reinterpret_cast<PFN_vkVoidFunction>(dispatch.present)) &&
           !isProxy(reinterpret_cast<PFN_vkVoidFunction>(dispatch.images));
}
} // namespace VulkanWsi
