#include "pch.h"

#include "DlssNr.h"
#include "DlssNrFeature_Vk.h"
#include "DlssNrFinished_Vk.h"
#include "DlssNr_PipelineUi.h"
#include "DlssNr_Upscaler.h"
#include "DlssNr_MenuSections.h"
#include "DlssNr_Placement.h"
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>

namespace DlssNr::MenuSections
{

static void RenderPlacement(Config* config)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    const bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    if (finishedPicture && enabled)
    {
        const auto feature = State::Instance().currentFeature;
        if (State::Instance().swapchainApi == API::Vulkan)
        {
            const auto status = DlssNr::LocalizeStatus(DlssNr::FinishedVkStatus());
            ImGui::TextWrapped("%s", status.c_str());
        }
        else if (feature && (feature->Api() != API::DX12 ||
                             (feature->IsWithDx12() && State::Instance().swapchainApi != API::DX11 &&
                              State::Instance().swapchainInteropApi != SwapchainInteropApi::Dx11wDx12)))
            ImGui::TextWrapped("此选项需要 DirectX 12，或标记为 w/Dx12 的 DirectX 11 放大器。");
        else
        {
            const auto status = DlssNr::LocalizeStatus(DlssNr::FinishedPictureStatus());
            ImGui::TextWrapped("%s", status.c_str());
        }
    }

    const auto placement =
        ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(), config->DlssNrDeferredDlss.value_or_default(),
                         config->DlssNrResidualAcrossRr.value_or_default(), finishedPicture);
    if (placement.deferred)
    {
        const auto status = DlssNr::LocalizeStatus(DlssNr::DeferredDlssStatus());
        ImGui::TextWrapped("独立放大：%s", status.c_str());
        ImGui::TextWrapped(finishedPicture ? "游戏通过 SR/RR 和自身效果处理干净输入，再将独立放大的 NR 编辑应用到最终画面。"
                                           : "游戏通过 SR/RR 处理干净输入，再将独立放大的 NR 编辑应用到放大结果。");
    }
}

static void RenderStatus(Config* config)
{
    const bool enabled = config->DlssNrEnabled.value_or_default();
    const bool finishedPicture = config->DlssNrFinishedPicture.value_or_default();
    const auto dx12 = ReadStatus(Backend::Dx12);
    const auto vk = ReadStatus(Backend::Vulkan);
    const bool vulkan = vk.running;

    // An existing model handle does not mean NR is enabled this frame.
    if (!enabled)
    {
        ImGui::TextDisabled("NR 已关闭。");
    }
    else if (!dx12.running && !vulkan)
    {
        const auto feature = State::Instance().currentFeature;
        const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        const auto& reason = nativeVk ? vk.failureReason : dx12.failureReason;

        if (!reason.empty())
        {
            const auto localizedReason = DlssNr::LocalizeStatus(reason);
            ImGui::TextWrapped("%s", localizedReason.c_str());
            ImGui::SameLine();

            if (nativeVk)
                ImGui::TextUnformatted("重启游戏以重试原生 Vulkan NR。");
            else if (ImGui::SmallButton("重试"))
                DlssNr::RetryAfterFailure();
        }
        else if (feature && feature->Api() == API::DX11 && !feature->IsWithDx12())
        {
            ImGui::TextWrapped("D3D11 下的 NR 需要 D3D12 桥接。请选择标记为 w/Dx12 的放大器并重启。");
        }
        else if (nativeVk && ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                              config->DlssNrDeferredDlss.value_or_default(),
                                              config->DlssNrResidualAcrossRr.value_or_default(), finishedPicture)
                                 .deferred)
        {
            ImGui::TextWrapped("独立编辑放大路径需要 DirectX 12 或其桥接。请关闭独立编辑放大以使用原生 Vulkan NR。");
        }
        else
            ImGui::TextUnformatted("等待放大器运行。");
    }
    else
    {
        const auto ms = vulkan ? vk.gpuTime : dx12.gpuTime;

        // Hiding the edit keeps model evaluation running.
        const char* runSuffix = !config->DlssNrApplyModel.value_or_default() ? "  （模型运行中，编辑已隐藏）" : "";

        // Keep the running indicator green, using the theme's HDR-adjusted text brightness.
        const auto textColor = ImGui::GetStyleColorVec4(ImGuiCol_Text);
        ImGui::PushStyleColor(ImGuiCol_Text,
                              ImVec4(textColor.x * 0.55f, textColor.y * 0.80f, textColor.z * 0.55f, textColor.w));
        if (ms.has_value())
            ImGui::Text("运行中%s - 已耗时 %.2f ms%s", vulkan ? "（原生 Vulkan）" : "", ms.value(), runSuffix);
        else if (vulkan)
            // Measured but not yet read: the first few frames are still in the query ring.
            ImGui::Text("原生 Vulkan 运行中 - %llu 帧%s", vk.frames, runSuffix);
        else
            ImGui::Text("运行中。%s", runSuffix);
        ImGui::PopStyleColor();

        ImGui::SameLine();
        ImGui::TextDisabled("(?)");
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("GPU 上 NR 开始到结束的时间，包括其他工作运行时的等待。\n请对比 FPS 判断对游戏性能的影响。");

        if (ms.has_value())
        {
            const auto& state = State::Instance();
            // The FG swapchain interval is between real game frames, not interpolated presents.
            // Native Vulkan has no DXGI timing, so use the existing overlay frame interval there.
            const double frameMs = state.swapchainApi == API::Vulkan
                                       ? (state.frameTimes.empty() ? 0.0 : state.frameTimes.back())
                                   : state.currentFG ? state.lastFGFrameTime
                                                     : state.presentFrameTime;
            PipelineUi::DrawTimingBar(ms.value(), frameMs);
        }
    }
}

} // namespace DlssNr::MenuSections

namespace DlssNr
{

void RenderMenu(Config* config, float menuResScale)
{
    using namespace MenuSections;
    ImGui::Spacing();
    if (auto header = ScopedCollapsingHeader("DLSS 神经渲染"); header.IsHeaderOpen())
    {
        ScopedIndent indent {};
        const float toggleGap = ImGui::GetStyle().ItemSpacing.x;
        const float toggleWidth = (ImGui::GetContentRegionAvail().x - toggleGap) * 0.5f;
        const float toggleRight = ImGui::GetCursorPosX() + toggleWidth + toggleGap;
        bool enabled = config->DlssNrEnabled.value_or_default();
        if (PipelineUi::CheckboxWrapped("启用神经渲染", &enabled, toggleWidth))
            config->DlssNrEnabled = enabled;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("启用 NR 处理。");

        bool applyModel = config->DlssNrApplyModel.value_or_default();
        if (PipelineUi::CheckboxWrapped("应用模型", &applyModel, toggleWidth))
            config->DlssNrApplyModel = applyModel;
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("显示或隐藏 NR 效果。隐藏时模型仍会运行。\n关闭神经渲染以停止 GPU 开销。");

        const auto feature = State::Instance().currentFeature;
        const bool rayReconstruction = feature && feature->GetUpscalerType() == Upscaler::DLSSD;
        bool finished = config->DlssNrFinishedPicture.value_or_default();
        auto placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                          config->DlssNrDeferredDlss.value_or_default(),
                                          config->DlssNrResidualAcrossRr.value_or_default(), finished);
        bool generateBefore = placement.beforeUpscale;
        const bool denoiseFirstOn = config->DlssNrDenoiseFirst.value_or_default();
        ImGui::SameLine(toggleRight);
        ImGui::BeginDisabled(placement.deferred || denoiseFirstOn);
        if (PipelineUi::CheckboxWrapped("在放大前生成模型", &generateBefore, toggleWidth))
        {
            config->DlssNrRunBeforeSr = generateBefore;
            config->DlssNrResidualAcrossRr = false;
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(placement.deferred ? "独立编辑路径始终在放大前生成。"
                              : denoiseFirstOn   ? "启用“先去噪”时由它决定位置。"
                                                 : "在游戏放大器之前运行 NR，包括 RR。");

        if (PipelineUi::CheckboxWrapped("将 NR 应用到最终画面", &finished, toggleWidth))
        {
            config->DlssNrFinishedPicture = finished;
            DlssNr::RetryAfterFailure();
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "在游戏效果和 HUD 之后应用 NR。提前生成时会通过独立放大器传递编辑结果。");

        placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                     config->DlssNrDeferredDlss.value_or_default(),
                                     config->DlssNrResidualAcrossRr.value_or_default(), finished);
        ImGui::SameLine(toggleRight);
        bool deferred = placement.deferred;
        if (PipelineUi::CheckboxWrapped("放大前生成，放大后应用", &deferred, toggleWidth))
        {
            config->DlssNrDeferredDlss = deferred;
            config->DlssNrResidualAcrossRr = false; // Clear the legacy alias when the unified option changes.
            if (deferred || finished)
                config->DlssNrRunBeforeSr = deferred;
        }
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip(
                "保持游戏 SR/RR 输入干净，只用独立的非 RR 后端放大 NR 编辑。"
                "\n在放大后应用；启用最终画面模式时则在呈现阶段应用。");
        ImGui::Spacing();

        // Denoise first. Written for people who did not design it: what it does, what it costs, what each knob is.
        placement = ResolvePlacement(config->DlssNrRunBeforeSr.value_or_default(),
                                     config->DlssNrDeferredDlss.value_or_default(),
                                     config->DlssNrResidualAcrossRr.value_or_default(), finished);
        const bool denoiseBlocked = placement.deferred || finished;
        bool denoiseFirst = config->DlssNrDenoiseFirst.value_or_default();
        ImGui::BeginDisabled(denoiseBlocked);
        if (PipelineUi::CheckboxWrapped("原生分辨率去噪，再 NR，再放大", &denoiseFirst, toggleWidth))
            config->DlssNrDenoiseFirst = denoiseFirst;
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip(
                denoiseBlocked
                    ? "请先关闭独立编辑和最终画面选项。"
                    : "在渲染分辨率再次运行游戏放大器（游戏使用 Ray Reconstruction 时使用它，否则使用 DLSS），"
                      "让 NR 看到干净、去噪、稳定的图像，而不是带噪声和抖动的图像。\n"
                      "NR 会编辑该图像；下方第二步决定编辑如何到达屏幕。\n"
                      "仅支持原生 DirectX 12，每帧会增加一次放大器处理。");
        if (denoiseFirst && !denoiseBlocked)
        {
            ScopedIndent denoiseIndent {};
            int step = std::clamp(config->DlssNrDenoiseFirstStep.value_or_default(), 0, 2);
            if (ImGui::Combo("第二步", &step,
                             "再次运行游戏放大器，抖动归零\0独立 DLSS SR，抖动归零\0将编辑应用到原始渲染，再运行游戏放大器\0"))
                config->DlssNrDenoiseFirstStep = step;
            HelpMarker(
                "再次运行游戏放大器：游戏放大器接收 NR 处理后的干净图像。简单，但第二次处理没有真实的子像素采样，"
                "画面可能比原生更柔和。\n\n"
                "独立 DLSS SR：同样的流程，但由独立的普通 DLSS 处理并替换输出，避免两次运行 Ray Reconstruction。\n\n"
                "应用到原始渲染：只提取干净图像中的 NR 变化，与原始渲染的抖动对齐后叠加。"
                "游戏放大器随后会看到携带编辑的真实采样。理论上最锐利；Ray Reconstruction 可能会柔化部分编辑。");
            if (step == 2)
            {
                int edit = config->DlssNrDenoiseFirstEdit.value_or_default() == 1 ? 1 : 0;
                if (ImGui::Combo("编辑方式", &edit, "差值（相加）\0比例（相乘）\0"))
                    config->DlssNrDenoiseFirstEdit = edit;
                HelpMarker("比例模式将每个原始像素乘以 NR/干净图像，让每个噪声采样保留自身色相，只改变亮度和色调，推荐使用。\n"
                           "差值模式将 NR - 干净图像作为绝对变化量。对变暗采样来说变化量会压过原采样，使其重新着色，"
                           "并可能让放大器接收到的色彩噪声大约翻倍。");
                if (edit == 1)
                {
                    bool fireflies = config->DlssNrDenoiseFirstFireflyGuard.value_or_default();
                    if (ImGui::Checkbox("不要乘以火花噪点", &fireflies))
                        config->DlssNrDenoiseFirstFireflyGuard = fireflies;
                    HelpMarker("比干净值亮很多的原始采样是路径追踪火花噪点，不是被照亮的像素。按比例相乘会使其更亮，"
                               "摄像机静止时游戏的 Ray Reconstruction 可能将这些闪烁保留在阴影中。启用后，这些采样会使用该像素本应有的绝对变化。"
                               "关闭即可对比。");
                }

                bool shift = config->DlssNrDenoiseFirstShift.value_or_default();
                if (ImGui::Checkbox("按当前帧抖动移动编辑", &shift))
                    config->DlssNrDenoiseFirstShift = shift;
                HelpMarker("干净图像位于稳定像素网格上；原始渲染有每帧变化的子像素抖动。按该抖动移动编辑即可对齐两者。"
                           "仅在对比时关闭。");
                if (shift)
                {
                    bool flip = config->DlssNrDenoiseFirstFlipJitter.value_or_default();
                    if (ImGui::Checkbox("翻转抖动方向", &flip))
                        config->DlssNrDenoiseFirstFlipJitter = flip;
                    HelpMarker("不同引擎对抖动值方向的约定不同。方向错误会让错位加倍而不是消除。"
                               "如果开启移动后边缘比关闭更糟，请翻转此选项。");
                    int kernel = std::clamp(config->DlssNrDenoiseFirstKernel.value_or_default(), 0, 2);
                    if (ImGui::Combo("编辑重采样", &kernel, "双线性\0Catmull-Rom\0Lanczos 2\0"))
                        config->DlssNrDenoiseFirstKernel = kernel;
                    HelpMarker("编辑在移动位置如何插值。双线性柔和且不会过冲；Catmull-Rom 锐利且过冲较少；Lanczos 2 最锐利，"
                               "可能在硬边缘附近出现振铃。");
                    bool clampEdit = config->DlssNrDenoiseFirstNeighbourhoodClamp.value_or_default();
                    if (ImGui::Checkbox("将编辑限制在邻域范围", &clampEdit))
                        config->DlssNrDenoiseFirstNeighbourhoodClamp = clampEdit;
                    HelpMarker("将插值编辑限制在最近四个源像素的范围内。可减少锐利滤镜的振铃，但会损失少量锐度。");
                }
            }
            const auto status = DlssNr::LocalizeStatus(DlssNr::DenoiseFirstStatus());
            ImGui::TextWrapped("先去噪：%s", status.c_str());
            ImGui::Spacing();
        }

        // placement is current: nothing it depends on changes in the denoise-first block above.
        const bool nativePrivateVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
        if (placement.deferred && !nativePrivateVk)
        {
            int backend = (int) GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default());
            if (ImGui::Combo("独立 NR 放大器", &backend, "DLSS\0FSR 2.2\0FSR（FidelityFX）\0XeSS\0"))
                config->DlssNrPrivateUpscaler = backend;
            HelpMarker(
                "只放大 NR 编辑，无论游戏是否使用 RR。FSR（FidelityFX）和 XeSS 需要对应运行库。");
        }

        PipelineUi::View view;
        view.privateUpscaler =
            PrivateUpscalerName(GetPrivateUpscaler(config->DlssNrPrivateUpscaler.value_or_default()));
        view.enabled = enabled;
        view.applyModel = config->DlssNrApplyModel.value_or_default();
        view.passes = config->DlssNrPasses.value_or_default();
        view.scalePercent = (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);
        view.rayReconstruction = rayReconstruction;
        if (finished)
            view.route = placement.deferred ? PipelineUi::Route::FinishedBefore : PipelineUi::Route::Finished;
        else if (placement.deferred)
            view.route = PipelineUi::Route::Deferred;
        else
            view.route = placement.beforeUpscale ? PipelineUi::Route::Before : PipelineUi::Route::After;

        static PipelineUi::Section selected = PipelineUi::Section::Placement;
        ImGui::Separator();
        PipelineUi::Draw(view, selected);
        ImGui::Separator();
        ImGui::Spacing();
        RenderStatus(config);
        ImGui::SeparatorText(PipelineUi::SectionName(selected));
        ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
        static constexpr void (*sections[])(Config*) = { RenderPlacement, RenderInput, RenderModel, RenderBlend };
        sections[(int) selected](config);
        ImGui::PopItemWidth();
        if (ImGui::CollapsingHeader("检查 NR"))
        {
            ImGui::PushItemWidth(std::min(220.0f * menuResScale, ImGui::GetContentRegionAvail().x * 0.42f));
            RenderInspect(config);
            ImGui::PopItemWidth();
        }
    }
}

} // namespace DlssNr
