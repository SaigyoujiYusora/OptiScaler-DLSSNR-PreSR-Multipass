#include "pch.h"
#include "menu_overlay_base.h"
#include "menu_overlay_vk.h"

#include <Util.h>
#include <Config.h>
#include <SysUtils.h>
#include <hooks/Streamline_Hooks.h>
#include <hooks/Vulkan_Hooks.h>

#include <imgui/imgui_impl_vulkan.h>
#include <imgui/imgui_impl_win32.h>

#include <vector>

// Vulkan overlay code adopted from here:
// https://gist.github.com/mem99/0ec31ca302927457f86b1d6756aaa8c4
// Need to check resize & recreate fixes

static bool _isInited = false;

static bool _vulkanObjectsCreated = false;
static std::recursive_mutex _vkPresentMutex;

// imgui stuff
struct ImGui_ImplVulkan_InitInfo _ImVulkan_Info = {};
struct ImGui_ImplVulkanH_Frame* _ImVulkan_Frames = VK_NULL_HANDLE;
static VkSemaphore* _ImVulkan_Semaphores = VK_NULL_HANDLE;
static VkRenderPass _vkRenderPass = VK_NULL_HANDLE;
static uint32_t _scImageCount = 0;
static uint32_t _overlayQueueFamily = UINT32_MAX;
static VkSwapchainKHR _overlaySwapchain = VK_NULL_HANDLE;
static VkDevice _overlayDevice = VK_NULL_HANDLE;
static bool _overlayFaulted = false;
static ULONG64 _frameCount;

MenuOverlayVk::PresentScope::PresentScope() { _vkPresentMutex.lock(); }
MenuOverlayVk::PresentScope::~PresentScope() { _vkPresentMutex.unlock(); }

static void SetVkObjectName(VkDevice device, VkInstance instance, VkObjectType objectType, uint64_t objectHandle,
                            const char* name)
{
    static PFN_vkSetDebugUtilsObjectNameEXT vkSetDebugUtilsObjectNameEXT = nullptr;

    if (vkSetDebugUtilsObjectNameEXT == nullptr)
        vkSetDebugUtilsObjectNameEXT = reinterpret_cast<PFN_vkSetDebugUtilsObjectNameEXT>(
            vkGetInstanceProcAddr(instance, "vkSetDebugUtilsObjectNameEXT"));

    VkDebugUtilsObjectNameInfoEXT info {};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_OBJECT_NAME_INFO_EXT;
    info.objectType = objectType;
    info.objectHandle = objectHandle;
    info.pObjectName = name;

    vkSetDebugUtilsObjectNameEXT(device, &info);
}

static void CreateVulkanObjects(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                const VkSwapchainCreateInfoKHR* pCreateInfo, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    if (device == VK_NULL_HANDLE || pCreateInfo == nullptr || *pSwapchain == VK_NULL_HANDLE)
    {
        LOG_WARN(
            "device({0:X}) == VK_NULL_HANDLE || pCreateInfo({1:X}) == nullptr || *pSwapchain({2:X}) == VK_NULL_HANDLE",
            (UINT64) device, (UINT64) pCreateInfo, (UINT64) *pSwapchain);
        return;
    }

    if (_vulkanObjectsCreated || _ImVulkan_Info.Device != VK_NULL_HANDLE)
    {
        LOG_DEBUG("existing Vulkan overlay state, releasing objects");

        MenuOverlayVk::DestroyVulkanObjects(false);

        _vulkanObjectsCreated = false;
    }

    // Initialize ImGui
    if (!MenuOverlayBase::IsInited() || MenuOverlayBase::Handle() != hwnd)
    {
        if (MenuOverlayBase::IsInited())
            MenuOverlayBase::Shutdown();

        LOG_DEBUG("MenuOverlayBase::Init");
        MenuOverlayBase::Init(hwnd, false);
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize.x = static_cast<float>(pCreateInfo->imageExtent.width);
    io.DisplaySize.y = static_cast<float>(pCreateInfo->imageExtent.height);

    VkResult result;

    VulkanHooks::QueueInfo queueInfo {};
    if (!VulkanHooks::GetGraphicsQueue(device, queueInfo))
    {
        LOG_WARN("Vulkan overlay: no created non-protected graphics queue was recorded");
        return;
    }

    if ((pCreateInfo->imageUsage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) == 0)
    {
        LOG_WARN("Vulkan overlay: swapchain has no COLOR_ATTACHMENT usage; skipping overlay resources");
        return;
    }

    bool verifiedWsi = false;
    const auto getSwapchainImages = VulkanHooks::GetSwapchainImages(device, *pSwapchain, &verifiedWsi);
    if (!getSwapchainImages)
    {
        LOG_WARN("Vulkan overlay: swapchain image entry point was not recorded for device/swapchain");
        return;
    }
    LOG_DEBUG("Vulkan overlay: WSI boundary verified: {}", verifiedWsi);

    // Get swapchain images through the same device WSI layer that created/presents this swapchain.
    std::vector<VkImage> images;
    for (;;)
    {
        uint32_t imageCount = 0;
        result = getSwapchainImages(device, *pSwapchain, &imageCount, nullptr);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
            return;
        }
        if (imageCount == 0)
        {
            LOG_WARN("Vulkan overlay: swapchain returned zero images");
            return;
        }
        images.resize(imageCount, VK_NULL_HANDLE);
        result = getSwapchainImages(device, *pSwapchain, &imageCount, images.data());
        if (result != VK_INCOMPLETE)
        {
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkGetSwapchainImagesKHR error: {0:X}", (UINT) result);
                return;
            }
            images.resize(imageCount);
            break;
        }
    }
    _scImageCount = static_cast<uint32_t>(images.size());

    _overlayDevice = device;
    _overlaySwapchain = *pSwapchain;
    _overlayQueueFamily = queueInfo.familyIndex;
    _overlayFaulted = false;
    _ImVulkan_Info.Instance = instance;
    _ImVulkan_Info.PhysicalDevice = pd;
    _ImVulkan_Info.Device = device;
    _ImVulkan_Info.QueueFamily = queueInfo.familyIndex;
    _ImVulkan_Info.Queue = queueInfo.queue;
    _ImVulkan_Info.ImageCount = _scImageCount;

    // Alloc ImGui frame structure/semaphores for every image.
    // For convenience, I am using ImGui_ImplVulkanH_Frame in imgui_impl_vulkan.h
    if (!_vulkanObjectsCreated)
    {
        _ImVulkan_Frames = (ImGui_ImplVulkanH_Frame*) IM_ALLOC(sizeof(ImGui_ImplVulkanH_Frame) * _scImageCount);
        if (_ImVulkan_Frames == nullptr)
        {
            LOG_ERROR("Vulkan overlay: unable to allocate per-image state for {} images", _scImageCount);
            return;
        }
        memset(_ImVulkan_Frames, 0, sizeof(ImGui_ImplVulkanH_Frame) * _scImageCount);

        _ImVulkan_Semaphores = (VkSemaphore*) IM_ALLOC(sizeof(VkSemaphore) * _scImageCount);
        if (_ImVulkan_Semaphores == nullptr)
        {
            LOG_ERROR("Vulkan overlay: unable to allocate per-image semaphore state for {} images", _scImageCount);
            IM_FREE(_ImVulkan_Frames);
            _ImVulkan_Frames = nullptr;
            return;
        }
        memset(_ImVulkan_Semaphores, 0, sizeof(VkSemaphore) * _scImageCount);
    }

    // Use a queue that the application actually created. Present-time submissions are validated
    // against the queue passed to QueuePresent and use that exact handle.
    const VkQueue queue = queueInfo.queue;
    const uint32_t queueFamily = queueInfo.familyIndex;

    // Create the render pool
    VkDescriptorPool pool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize sampler_pool_size = {};
        sampler_pool_size.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        sampler_pool_size.descriptorCount = 8; // required by ImGui 1.92

        VkDescriptorPoolCreateInfo desc_pool_info = {};
        desc_pool_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        desc_pool_info.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
        desc_pool_info.maxSets = 8;
        desc_pool_info.poolSizeCount = 1;
        desc_pool_info.pPoolSizes = &sampler_pool_size;

        result = vkCreateDescriptorPool(device, &desc_pool_info, NULL, &pool);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateDescriptorPool error: {0:X}", (UINT) result);
            return;
        }
        _ImVulkan_Info.DescriptorPool = pool;
    }

    // Create the render pass
    {
        VkAttachmentDescription attachment_desc = {};

        attachment_desc.format = pCreateInfo->imageFormat;
        attachment_desc.samples = VK_SAMPLE_COUNT_1_BIT;
        attachment_desc.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        attachment_desc.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        attachment_desc.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        attachment_desc.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        attachment_desc.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        attachment_desc.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        VkAttachmentReference color_attachment = {};
        color_attachment.attachment = 0;
        color_attachment.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

        VkSubpassDescription subpass = {};
        subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        subpass.colorAttachmentCount = 1;
        subpass.pColorAttachments = &color_attachment;

        VkSubpassDependency dependencies[2] = {};
        dependencies[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[0].dstSubpass = 0;
        dependencies[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].srcSubpass = 0;
        dependencies[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dependencies[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dependencies[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        dependencies[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dependencies[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;

        VkRenderPassCreateInfo render_pass_info = {};
        render_pass_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
        render_pass_info.attachmentCount = 1;
        render_pass_info.pAttachments = &attachment_desc;
        render_pass_info.subpassCount = 1;
        render_pass_info.pSubpasses = &subpass;
        render_pass_info.dependencyCount = 2;
        render_pass_info.pDependencies = dependencies;

        result = vkCreateRenderPass(device, &render_pass_info, NULL, &_vkRenderPass);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkCreateRenderPass error: {0:X}", (UINT) result);
            return;
        }
        _ImVulkan_Info.RenderPass = _vkRenderPass;
    }

    // Create The Image Views
    {
        VkImageViewCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        info.viewType = VK_IMAGE_VIEW_TYPE_2D;

        info.format = pCreateInfo->imageFormat;
        info.components.r = VK_COMPONENT_SWIZZLE_R;
        info.components.g = VK_COMPONENT_SWIZZLE_G;
        info.components.b = VK_COMPONENT_SWIZZLE_B;
        info.components.a = VK_COMPONENT_SWIZZLE_A;

        VkImageSubresourceRange image_range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        info.subresourceRange = image_range;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            fd->Backbuffer = images[i];
            info.image = fd->Backbuffer;

            result = vkCreateImageView(device, &info, NULL, &fd->BackbufferView);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateImageView error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_IMAGE_VIEW, (UINT64) fd->BackbufferView,
                            "ImGui Backbuffer View");
#endif
        }
    }

    // Create frame Buffer
    {
        VkImageView attachment[1];
        VkFramebufferCreateInfo info = {};
        info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        info.renderPass = _vkRenderPass;
        info.attachmentCount = 1;
        info.pAttachments = attachment;

        info.width = pCreateInfo->imageExtent.width;
        info.height = pCreateInfo->imageExtent.height;

        info.layers = 1;

        for (uint32_t i = 0; i < _scImageCount; i++)
        {
            ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
            attachment[0] = fd->BackbufferView;
            result = vkCreateFramebuffer(device, &info, NULL, &fd->Framebuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFramebuffer error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FRAMEBUFFER, (UINT64) fd->Framebuffer,
                            "ImGui Backbuffer Framebuffer");
#endif
        }
    }

    // Create command pools, command buffers, fences, and semaphores for every image
    for (uint32_t i = 0; i < _scImageCount; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];
        VkSemaphore* fsd = &_ImVulkan_Semaphores[i];
        {
            VkCommandPoolCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = queueFamily;
            result = vkCreateCommandPool(device, &info, NULL, &fd->CommandPool);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateCommandPool error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_POOL, (UINT64) fd->CommandPool,
                            "ImGui Backbuffer Command Pool");
#endif
        }

        {
            VkCommandBufferAllocateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            info.commandPool = fd->CommandPool;
            info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            info.commandBufferCount = 1;
            result = vkAllocateCommandBuffers(device, &info, &fd->CommandBuffer);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkAllocateCommandBuffers error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_COMMAND_BUFFER, (UINT64) fd->CommandBuffer,
                            "ImGui Backbuffer Command Buffer");
#endif
        }

        {
            VkFenceCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
            info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
            result = vkCreateFence(device, &info, NULL, &fd->Fence);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateFence error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_FENCE, (UINT64) fd->Fence, "ImGui Backbuffer Fence");
#endif
        }

        {
            VkSemaphoreCreateInfo info = {};
            info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
            result = vkCreateSemaphore(device, &info, NULL, fsd);
            if (result != VK_SUCCESS)
            {
                LOG_ERROR("vkCreateSemaphore error: {0:X}", (UINT) result);
                return;
            }

#ifdef VULKAN_DEBUG_LAYER
            SetVkObjectName(device, instance, VK_OBJECT_TYPE_SEMAPHORE, (UINT64) fsd, "ImGui Backbuffer Semaphore");
#endif
        }
    }

    // Initialize ImGui and upload fonts
    {
        _ImVulkan_Info.Instance = instance;
        _ImVulkan_Info.PhysicalDevice = pd;
        _ImVulkan_Info.Device = device;
        _ImVulkan_Info.QueueFamily = queueFamily;
        _ImVulkan_Info.Queue = queue;
        _ImVulkan_Info.DescriptorPool = pool;
        _ImVulkan_Info.Subpass = 0;
        _ImVulkan_Info.MinImageCount = pCreateInfo->minImageCount;
        _ImVulkan_Info.ImageCount = _scImageCount;
        _ImVulkan_Info.Allocator = NULL;
        _ImVulkan_Info.RenderPass = _vkRenderPass;

        bool initResult = ImGui_ImplVulkan_Init(&_ImVulkan_Info);
        LOG_DEBUG("ImGui_ImplVulkan_Init result: {}", initResult);

        if (!initResult)
            return;

        // Upload Fonts
        // Use any command queue
        VkCommandPool command_pool = _ImVulkan_Frames[0].CommandPool;
        VkCommandBuffer command_buffer = _ImVulkan_Frames[0].CommandBuffer;
        result = vkResetCommandPool(device, command_pool, 0);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        VkCommandBufferBeginInfo begin_info = {};
        begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin_info.flags |= VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(command_buffer, &begin_info);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkBeginCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        // initResult = ImGui_ImplVulkan_CreateFontsTexture();
        // LOG_DEBUG("ImGui_ImplVulkan_CreateFontsTexture result: {}", initResult);

        VkSubmitInfo end_info = {};
        end_info.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        end_info.commandBufferCount = 1;
        end_info.pCommandBuffers = &command_buffer;

        result = vkEndCommandBuffer(command_buffer);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkEndCommandBuffer error: {0:X}", (UINT) result);
            return;
        }

        result = vkQueueSubmit(queue, 1, &end_info, VK_NULL_HANDLE);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkQueueSubmit error: {0:X}", (UINT) result);
            return;
        }

        result = vkDeviceWaitIdle(device);
        if (result != VK_SUCCESS)
        {
            LOG_ERROR("vkDeviceWaitIdle error: {0:X}", (UINT) result);
            return;
        }
    }

    _vulkanObjectsCreated = true;
    State::Instance().menuOverlayIsVulkan = true;
    LOG_FUNC_RESULT(_vulkanObjectsCreated);
}

void MenuOverlayVk::DestroyVulkanObjects(bool shutdown)
{
    std::lock_guard lock(_vkPresentMutex);
    State::Instance().menuOverlayIsVulkan = false;
    if (_ImVulkan_Info.Device == VK_NULL_HANDLE)
    {
        _vulkanObjectsCreated = false;
        _overlayFaulted = false;
        _overlayQueueFamily = UINT32_MAX;
        _overlaySwapchain = VK_NULL_HANDLE;
        _overlayDevice = VK_NULL_HANDLE;
        _scImageCount = 0;
        _ImVulkan_Info = {};
        _isInited = false;
        return;
    }

    if (!shutdown)
        LOG_FUNC();

    auto result = vkDeviceWaitIdle(_ImVulkan_Info.Device);
    if (result != VK_SUCCESS && !shutdown)
        LOG_WARN("vkDeviceWaitIdle error: {0:X}", (UINT) result);

    if (ImGui::GetCurrentContext() != nullptr && ImGui::GetIO().BackendRendererUserData != nullptr)
        ImGui_ImplVulkan_Shutdown(false);

    for (uint32_t i = 0; i < _ImVulkan_Info.ImageCount && _ImVulkan_Frames != nullptr; i++)
    {
        ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[i];

        // Framebuffers own image-view references, so release them before the views.
        if (fd->Framebuffer != VK_NULL_HANDLE)
        {
            vkDestroyFramebuffer(_ImVulkan_Info.Device, fd->Framebuffer, VK_NULL_HANDLE);
            fd->Framebuffer = VK_NULL_HANDLE;
        }

        if (fd->Fence != VK_NULL_HANDLE)
        {
            vkDestroyFence(_ImVulkan_Info.Device, fd->Fence, VK_NULL_HANDLE);
            fd->Fence = VK_NULL_HANDLE;
        }

        if (fd->CommandBuffer != VK_NULL_HANDLE)
        {
            vkFreeCommandBuffers(_ImVulkan_Info.Device, fd->CommandPool, 1, &fd->CommandBuffer);
            fd->CommandBuffer = VK_NULL_HANDLE;
        }

        if (fd->CommandPool != VK_NULL_HANDLE)
        {
            vkDestroyCommandPool(_ImVulkan_Info.Device, fd->CommandPool, VK_NULL_HANDLE);
            fd->CommandPool = VK_NULL_HANDLE;
        }

        if (fd->BackbufferView != VK_NULL_HANDLE)
        {
            vkDestroyImageView(_ImVulkan_Info.Device, fd->BackbufferView, VK_NULL_HANDLE);
            fd->BackbufferView = VK_NULL_HANDLE;
        }

        if (_ImVulkan_Semaphores != nullptr && _ImVulkan_Semaphores[i] != VK_NULL_HANDLE)
        {
            vkDestroySemaphore(_ImVulkan_Info.Device, _ImVulkan_Semaphores[i], VK_NULL_HANDLE);
            _ImVulkan_Semaphores[i] = VK_NULL_HANDLE;
        }
    }

    if (_vkRenderPass != VK_NULL_HANDLE)
    {
        vkDestroyRenderPass(_ImVulkan_Info.Device, _vkRenderPass, VK_NULL_HANDLE);
        _vkRenderPass = VK_NULL_HANDLE;
    }

    if (_ImVulkan_Info.DescriptorPool != VK_NULL_HANDLE)
    {
        vkDestroyDescriptorPool(_ImVulkan_Info.Device, _ImVulkan_Info.DescriptorPool, VK_NULL_HANDLE);
        _ImVulkan_Info.DescriptorPool = VK_NULL_HANDLE;
    }

    if (_ImVulkan_Frames != nullptr)
    {
        IM_FREE(_ImVulkan_Frames);
        _ImVulkan_Frames = nullptr;
    }
    if (_ImVulkan_Semaphores != nullptr)
    {
        IM_FREE(_ImVulkan_Semaphores);
        _ImVulkan_Semaphores = nullptr;
    }

    _ImVulkan_Info = {};

    _vulkanObjectsCreated = false;
    _overlayFaulted = false;
    _overlayQueueFamily = UINT32_MAX;
    _overlaySwapchain = VK_NULL_HANDLE;
    _overlayDevice = VK_NULL_HANDLE;
    _scImageCount = 0;
    _frameCount = 0;
    _isInited = false;
}

void MenuOverlayVk::DestroySwapchain(VkDevice device, VkSwapchainKHR swapchain)
{
    std::lock_guard lock(_vkPresentMutex);
    if (_overlayDevice != device || _overlaySwapchain != swapchain)
        return;

    DestroyVulkanObjects(false);
}

bool MenuOverlayVk::QueuePresent(VkQueue queue, VkPresentInfoKHR* pPresentInfo)
{
    LOG_FUNC();

    std::lock_guard lock(_vkPresentMutex);

    if (!_vulkanObjectsCreated)
        return true;

    if (!MenuOverlayBase::IsInited() || _ImVulkan_Info.Device == VK_NULL_HANDLE)
        return true;

    if (pPresentInfo == nullptr || pPresentInfo->swapchainCount != 1 || pPresentInfo->pSwapchains == nullptr ||
        pPresentInfo->pImageIndices == nullptr)
        return true;

    if (_overlayFaulted || pPresentInfo->pSwapchains[0] != _overlaySwapchain)
        return true;

    const uint32_t imageIndex = pPresentInfo->pImageIndices[0];
    if (imageIndex >= _scImageCount || _ImVulkan_Frames == nullptr || _ImVulkan_Semaphores == nullptr)
        return true;

    VulkanHooks::QueueInfo presentQueueInfo {};
    bool verifiedWsi = false;
    if (!VulkanHooks::GetPresentQueue(_ImVulkan_Info.Device, queue, _overlaySwapchain, presentQueueInfo,
                                      &verifiedWsi) ||
        presentQueueInfo.familyIndex != _overlayQueueFamily)
    {
        LOG_DEBUG("Vulkan overlay: present queue/swapchain identity is not compatible; skipping overlay");
        return true;
    }

    if (!ImGui_ImplVulkan_SetPresentQueue(queue, presentQueueInfo.familyIndex))
    {
        LOG_DEBUG("Vulkan overlay: ImGui backend rejected present queue family; skipping overlay");
        return true;
    }

    LOG_DEBUG("rendering menu, swapchain count: {0}", pPresentInfo->swapchainCount);

    ImGuiIO& io = ImGui::GetIO();
    (void) io;
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

    _frameCount++;

    ImGui_ImplVulkan_NewFrame();

    if (State::Instance().delayMenuRenderBy > 0)
        State::Instance().delayMenuRenderBy--;

    const bool renderMenu = MenuOverlayBase::RenderMenu();
    // Run on the Vulkan present thread, after input changed visibility and before writing
    // the swapchain image. A failed FG pause must not race the overlay's GPU submission.
    const bool nativeFgAllowsOverlay = StreamlineHooks::SyncNativeVulkanDlssgMenu(renderMenu, verifiedWsi);
    if (!renderMenu)
        return true;

    // Close the ImGui frame before any error path can return. The GPU submission below may still
    // be skipped, but the next NewFrame must never observe an unrendered frame.
    ImGui::Render();

    // WSI validation is independent of catching slInit. Late injection can miss that
    // call, while the legacy DLSSG options hook still resets the menu delay each frame.
    // Passive drawing at this verified output boundary does not need the route flag.
    const bool verifiedPassiveBoundary = verifiedWsi && !MenuOverlayBase::IsVisible();
    if (State::Instance().delayMenuRenderBy != 0 && verifiedPassiveBoundary)
    {
        static bool reportedDelayBypass = false;
        if (!reportedDelayBypass)
        {
            LOG_INFO("Vulkan overlay: passive WSI output bypasses legacy menu delay; native SL init detected: {}",
                     StreamlineHooks::IsNativeVulkanDlssg());
            reportedDelayBypass = true;
        }
    }
    if ((State::Instance().delayMenuRenderBy != 0 && !verifiedPassiveBoundary) || !nativeFgAllowsOverlay)
        return true;

    ImGui_ImplVulkanH_Frame* fd = &_ImVulkan_Frames[imageIndex];
    auto result = vkWaitForFences(_ImVulkan_Info.Device, 1, &fd->Fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkWaitForFences failed: {0:X}", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    result = vkResetFences(_ImVulkan_Info.Device, 1, &fd->Fence);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkResetFences failed: {0:X}", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    result = vkResetCommandPool(_ImVulkan_Info.Device, fd->CommandPool, 0);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkResetCommandPool failed: {0:X}", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    VkCommandBufferBeginInfo beginInfo {};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    result = vkBeginCommandBuffer(fd->CommandBuffer, &beginInfo);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkBeginCommandBuffer failed: {0:X}", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    VkRenderPassBeginInfo renderPassInfo {};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = _vkRenderPass;
    renderPassInfo.framebuffer = fd->Framebuffer;
    renderPassInfo.renderArea.extent.width = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.x);
    renderPassInfo.renderArea.extent.height = static_cast<uint32_t>(ImGui::GetIO().DisplaySize.y);
    vkCmdBeginRenderPass(fd->CommandBuffer, &renderPassInfo, VK_SUBPASS_CONTENTS_INLINE);

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), fd->CommandBuffer, VK_NULL_HANDLE, imageIndex);
    vkCmdEndRenderPass(fd->CommandBuffer);

    result = vkEndCommandBuffer(fd->CommandBuffer);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkEndCommandBuffer failed: {0:X}", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    if (pPresentInfo->waitSemaphoreCount != 0 && pPresentInfo->pWaitSemaphores == nullptr)
    {
        LOG_ERROR("Vulkan overlay: present reported wait semaphores without an array");
        _overlayFaulted = true;
        return true;
    }

    std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
    VkSubmitInfo submitInfo {};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
    submitInfo.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages.empty() ? nullptr : waitStages.data();
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &fd->CommandBuffer;
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = &_ImVulkan_Semaphores[imageIndex];

    result = vkQueueSubmit(queue, 1, &submitInfo, fd->Fence);
    if (result != VK_SUCCESS)
    {
        LOG_ERROR("Vulkan overlay: vkQueueSubmit failed: {0:X}; disabling until swapchain recreation", (UINT) result);
        _overlayFaulted = true;
        return true;
    }

    // Only replace the application's present wait list after the overlay submit succeeded.
    pPresentInfo->waitSemaphoreCount = 1;
    pPresentInfo->pWaitSemaphores = &_ImVulkan_Semaphores[imageIndex];

    if (verifiedPassiveBoundary)
    {
        static bool reportedPassiveSubmit = false;
        if (!reportedPassiveSubmit)
        {
            LOG_INFO("Vulkan overlay: passive overlay submitted on verified WSI output");
            reportedPassiveSubmit = true;
        }
    }

    return true;
}

void MenuOverlayVk::CreateSwapchain(VkDevice device, VkPhysicalDevice pd, VkInstance instance, HWND hwnd,
                                    const VkSwapchainCreateInfoKHR* pCreateInfo,
                                    const VkAllocationCallbacks* pAllocator, VkSwapchainKHR* pSwapchain)
{
    LOG_FUNC();

    std::lock_guard lock(_vkPresentMutex);

    if (MenuOverlayBase::Handle() != hwnd)
    {
        LOG_DEBUG("MenuOverlayBase::Handle() != _hwnd");

        if (MenuOverlayBase::IsInited())
        {
            if (_ImVulkan_Info.Device != VK_NULL_HANDLE)
                DestroyVulkanObjects(false);
            LOG_DEBUG("MenuOverlayBase::Shutdown();");
            MenuOverlayBase::Shutdown();
        }

        LOG_DEBUG("MenuOverlayBase::Init({0:X})", (UINT64) hwnd);
        MenuOverlayBase::Init(hwnd, false);
    }

    CreateVulkanObjects(device, pd, instance, hwnd, pCreateInfo, pSwapchain);

    if (_ImVulkan_Info.Device != VK_NULL_HANDLE)
    {
        _isInited = true;
        MenuOverlayBase::VulkanReady();
        LOG_DEBUG("Vulkan ready");
    }
}
