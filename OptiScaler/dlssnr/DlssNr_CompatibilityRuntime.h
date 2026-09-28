#pragma once

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_vk.h>
#include <filesystem>
#include <memory>

struct ID3D12Device;
struct ID3D12GraphicsCommandList;

namespace DlssNr
{
// Own this alongside the feature until its recorded GPU work has retired.
// The driver still owns parameter allocation; only NR model calls use this backend.
class CompatibilityRuntime
{
  public:
    using Allocate = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
    using Destroy = NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);

    static std::shared_ptr<CompatibilityRuntime> TryOpen(ID3D12Device* device);
    static std::shared_ptr<CompatibilityRuntime> Open(const std::filesystem::path& path, ID3D12Device* device,
                                                      Allocate allocate, Destroy destroy,
                                                      const std::filesystem::path& dataPath = {});
    static std::shared_ptr<CompatibilityRuntime> TryOpenVulkan(VkInstance instance, VkPhysicalDevice physicalDevice,
                                                               VkDevice device);
    static std::shared_ptr<CompatibilityRuntime> OpenVulkan(const std::filesystem::path& path, VkInstance instance,
                                                            VkPhysicalDevice physicalDevice, VkDevice device,
                                                            Allocate allocate, Destroy destroy,
                                                            const std::filesystem::path& dataPath = {});
    ~CompatibilityRuntime();
    CompatibilityRuntime(const CompatibilityRuntime&) = delete;
    CompatibilityRuntime& operator=(const CompatibilityRuntime&) = delete;
    NVSDK_NGX_Result Create(ID3D12GraphicsCommandList*, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    NVSDK_NGX_Result Create(VkCommandBuffer, NVSDK_NGX_Parameter*, NVSDK_NGX_Handle**);
    NVSDK_NGX_Result Evaluate(ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*);
    NVSDK_NGX_Result Evaluate(VkCommandBuffer, const NVSDK_NGX_Handle*, NVSDK_NGX_Parameter*);
    NVSDK_NGX_Result Release(NVSDK_NGX_Handle*);

  private:
    struct Module;
    static std::shared_ptr<Module> LoadModule(const std::filesystem::path& path);
    std::shared_ptr<Module> module;
    ID3D12Device* device = nullptr;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice vkDevice = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* capabilities = nullptr;
    Destroy destroyParameters = nullptr;
    bool initialized = false;
    bool vulkan = false;
    CompatibilityRuntime() = default;
};
} // namespace DlssNr
