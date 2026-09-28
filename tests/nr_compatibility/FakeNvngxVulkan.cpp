#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>

#include <vulkan/vulkan.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_params.h>

namespace
{
struct Stats
{
    unsigned init = 0;
    unsigned create = 0;
    unsigned evaluate = 0;
    unsigned release = 0;
    unsigned shutdown = 0;
    unsigned long long applicationId = 0;
    unsigned version = 0;
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    const NVSDK_NGX_FeatureCommonInfo* featureInfo = nullptr;
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    NVSDK_NGX_Parameter* initParameters = nullptr;
    NVSDK_NGX_Parameter* createParameters = nullptr;
    NVSDK_NGX_Parameter* evaluateParameters = nullptr;
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Feature featureId = {};
};

Stats g_stats;
NVSDK_NGX_Handle g_feature { 0x4E52564Bu };

// Keep GetModuleFileNameW in the fixture's import table. The production runtime
// patches this named import for the real NGX loader's caller-name check.
void ObserveModulePath()
{
    HMODULE self = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&ObserveModulePath), &self))
    {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(self, path, static_cast<DWORD>(std::size(path)));
    }
}
} // namespace

extern "C" __declspec(dllexport) void FakeResetStats()
{
    g_stats = {};
    g_feature.Id = 0x4E52564Bu;
}

extern "C" __declspec(dllexport) Stats FakeGetStats() { return g_stats; }

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_Init_Ext(unsigned long long applicationId, const wchar_t*, VkInstance instance,
                          VkPhysicalDevice physicalDevice, VkDevice device, const void* featureInfo, unsigned version)
{
    ObserveModulePath();
    ++g_stats.init;
    g_stats.applicationId = applicationId;
    g_stats.version = static_cast<unsigned>(version);
    g_stats.instance = instance;
    g_stats.physicalDevice = physicalDevice;
    g_stats.device = device;
    g_stats.featureInfo = reinterpret_cast<const NVSDK_NGX_FeatureCommonInfo*>(featureInfo);
    return NVSDK_NGX_Result_Success;
}

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV
NVSDK_NGX_VULKAN_CreateFeature(VkCommandBuffer commandBuffer, NVSDK_NGX_Feature featureId,
                               NVSDK_NGX_Parameter* parameters, NVSDK_NGX_Handle** outHandle)
{
    ObserveModulePath();
    ++g_stats.create;
    g_stats.commandBuffer = commandBuffer;
    g_stats.featureId = featureId;
    g_stats.createParameters = parameters;
    g_stats.feature = &g_feature;
    if (outHandle)
        *outHandle = &g_feature;
    return NVSDK_NGX_Result_Success;
}

#ifndef FAKE_MISSING_EVALUATE
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_EvaluateFeature(
    VkCommandBuffer commandBuffer, const NVSDK_NGX_Handle* feature, NVSDK_NGX_Parameter* parameters, void*)
{
    ObserveModulePath();
    ++g_stats.evaluate;
    g_stats.commandBuffer = commandBuffer;
    g_stats.feature = const_cast<NVSDK_NGX_Handle*>(feature);
    g_stats.evaluateParameters = parameters;
    return feature == &g_feature ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotFound;
}
#endif

extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_ReleaseFeature(NVSDK_NGX_Handle* feature)
{
    ObserveModulePath();
    ++g_stats.release;
    g_stats.feature = feature;
    return feature == &g_feature ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_FeatureNotFound;
}

#ifndef FAKE_MISSING_SHUTDOWN
extern "C" __declspec(dllexport) NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_VULKAN_Shutdown1(VkDevice device)
{
    ObserveModulePath();
    ++g_stats.shutdown;
    return device == g_stats.device ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_FAIL_InvalidParameter;
}
#endif
