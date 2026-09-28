// Uses the production pre-SR pipeline functions and the actual Evaluate tail.
// The shader/upscaler record resource handoff and execution order without a GPU.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <map>
#include <vector>
using VkCommandBuffer = void*;
using VkInstance = void*;
constexpr auto VK_NULL_HANDLE = nullptr;
using VkImageLayout = int;
constexpr int VK_IMAGE_LAYOUT_GENERAL = 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL = 2;
constexpr int NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW = 1;
constexpr const char* NVSDK_NGX_Parameter_Color = "color";
constexpr const char* NVSDK_NGX_Parameter_Output = "output";
struct VkImageInfo
{
    uintptr_t ImageView = 0, Image = 0;
    int SubresourceRange = 0, Format = 0;
    unsigned Width = 0, Height = 0;
};
struct NVSDK_NGX_Resource_VK
{
    int Type = NVSDK_NGX_RESOURCE_VK_TYPE_VK_IMAGEVIEW;
    struct
    {
        VkImageInfo ImageViewInfo;
    } Resource;
    bool ReadWrite = false;
};
struct NVSDK_NGX_Parameter
{
    std::map<const char*, void*> values;
    void Get(const char* key, void** p) { *p = values.at(key); }
    void Get(const char* key, NVSDK_NGX_Resource_VK** p) { *p = static_cast<NVSDK_NGX_Resource_VK*>(values.at(key)); }
    void Set(const char* key, void* p) { values[key] = p; }
};
struct DlssNrFrameInfo_Vk
{
    bool BeforeUpscale = false;
};
std::vector<int> events;
struct DlssNr_Vk
{
    bool succeeds = true;
    VkImageInfo scratch { 20, 20, 0, 0, 1280, 720 };
    VkImageInfo PrepareInput(VkCommandBuffer, const VkImageInfo&) { return scratch; }
    bool Dispatch(VkCommandBuffer, const VkImageInfo& input, const VkImageInfo&, const VkImageInfo&,
                  const VkImageInfo& output, const DlssNrFrameInfo_Vk& frame, VkInstance, VkImageLayout layout)
    {
        events.push_back(1); // NR records first
        assert(frame.BeforeUpscale && input.Image == 10 && output.Image == 20);
        assert(input.Width == 1280 && output.Width == 1280);
        assert(layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        return succeeds;
    }
    void SetImageLayout(VkCommandBuffer, uintptr_t image, VkImageLayout from, VkImageLayout to, int)
    {
        assert(image == 20);
        assert((from == VK_IMAGE_LAYOUT_GENERAL && to == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) ||
               (from == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL && to == VK_IMAGE_LAYOUT_GENERAL));
    }
};
#include "vulkan_shader_pipeline.inc"
namespace DlssNr
{
#include "vulkan_presr_production.inc"
}
struct Host
{
    DlssNr_Vk shader;
    DlssNr_Vk* NeuralRendering = &shader;
    unsigned _frameCount = 0;
    bool upscaleSuccess = true;
    uintptr_t observedInput = 0;
    bool EvaluateInternal(VkCommandBuffer, NVSDK_NGX_Parameter* params)
    {
        events.push_back(2); // DLSS/SR consumes the substituted input
        observedInput = DlssNr::ParameterImage(params, NVSDK_NGX_Parameter_Color).Image;
        return upscaleSuccess;
    }
    bool Run(NVSDK_NGX_Parameter* InParameters, bool nrBeforeUpscale)
    {
        VkCommandBuffer InCmdBuffer = nullptr;
        VkInstance Instance = nullptr;
        VkImageInfo nrDepth {}, nrMotion {};
        DlssNrFrameInfo_Vk nrFrame {};
        ShaderPipeline_Vk pipeline;
        DlssNr::ScopedVkParameters scopedParameters(InParameters);
#include "vulkan_evaluate_tail.inc"
    }
};
int main()
{
    auto color = DlssNr::WrapImage({ 10, 10, 0, 0, 1280, 720 }, false);
    auto output = DlssNr::WrapImage({ 30, 30, 0, 0, 2560, 1440 }, true);
    NVSDK_NGX_Parameter params;
    params.Set(NVSDK_NGX_Parameter_Color, &color);
    params.Set(NVSDK_NGX_Parameter_Output, &output);
    for (bool nrSuccess : { false, true })
        for (bool srSuccess : { false, true })
        {
            Host host;
            host.shader.succeeds = nrSuccess;
            host.upscaleSuccess = srSuccess;
            events.clear();
            assert(host.Run(&params, true) == srSuccess);
            assert((events == std::vector<int> { 1, 2 }));
            assert(host.observedInput == (nrSuccess ? 20u : 10u));
            assert(params.values.at(NVSDK_NGX_Parameter_Color) == &color);
            assert(params.values.at(NVSDK_NGX_Parameter_Output) == &output);
            assert(host._frameCount == (srSuccess ? 1u : 0u));
        }
    Host host;
    events.clear();
    assert(host.Run(&params, false));
    assert((events == std::vector<int> { 2 }) && host.observedInput == 10);
    std::puts("PASS: production Vulkan pre-SR runs NR before SR, hands off the scratch image, restores input on "
              "success/failure");
}
