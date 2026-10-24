#include "pch.h"
#include "DlssNr_MenuSections.h"
#include "DlssNr_Placement.h"
#include "DlssNr_Status.h"
#include <shaders/dlssnr/DlssNr_Spatial.h>
#include <Config.h>
#include <menu/menu_common.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <unordered_map>

namespace DlssNr::MenuSections
{
template <typename Option> static void Checkbox(const char* label, Option& option)
{
    bool value = option.value_or_default();
    if (ImGui::Checkbox(label, &value))
        option = value;
}

template <typename Option>
static void Slider(const char* label, Option& option, float minimum, float maximum, const char* format = "%.2f",
                   std::optional<float> reset = {}, ImGuiSliderFlags flags = 0)
{
    float value = option.value_or_default();
    if (ImGui::SliderFloat(label, &value, minimum, maximum, format, flags))
        option = value;
    if (reset)
    {
        ImGui::SameLine();
        ImGui::PushID(label);
        if (ImGui::SmallButton("重置"))
            option = *reset;
        ImGui::PopID();
    }
}

static void StoreSpatial(Config* config, const Spatial::Settings& value)
{
    config->DlssNrSpatialCenterX = value.centerX;
    config->DlssNrSpatialCenterY = value.centerY;
    config->DlssNrSpatialWorkX = value.workX;
    config->DlssNrSpatialWorkY = value.workY;
    config->DlssNrSpatialOffsetX = value.offsetX;
    config->DlssNrSpatialOffsetY = value.offsetY;
    config->DlssNrSpatialShiftX = value.shiftX;
    config->DlssNrSpatialShiftY = value.shiftY;
}

// Used only for user edits. Invalid INI values remain visible as a runtime fallback.
static void ConstrainSpatialControls(Spatial::Settings& value, float scale)
{
    const float minimumWork = Spatial::MinimumWorkPercent(scale);
    auto axis = [&](float& center, float& work, float& offset)
    {
        center = std::clamp(std::isfinite(center) ? center : 80.0f, 1.0f, 99.5f);
        work = std::clamp(std::isfinite(work) ? work : 90.0f, std::max(minimumWork, center + 0.5f), 100.0f);
        const float limit = Spatial::MaxCenterOffset(center);
        offset = std::clamp(std::isfinite(offset) ? offset : 0.0f, -limit, limit);
    };
    axis(value.centerX, value.workX, value.offsetX);
    axis(value.centerY, value.workY, value.offsetY);
    const auto x = Spatial::WorkShiftLimits(value, false);
    const auto y = Spatial::WorkShiftLimits(value, true);
    value.shiftX = std::clamp(std::isfinite(value.shiftX) ? value.shiftX : 0.0f, x.first, x.second);
    value.shiftY = std::clamp(std::isfinite(value.shiftY) ? value.shiftY : 0.0f, y.first, y.second);
}

static void RenderSpatial(Config* config)
{
    Checkbox("周边压缩", config->DlssNrSpatialCompression);
    HelpMarker(
        "保留中心区域更多模型细节并压缩边缘。模型分辨率仍会缩放整张图像。");
    bool preview = config->DlssNrDebugView.value_or_default() == 4;
    if (ImGui::Checkbox("预览", &preview))
        config->DlssNrDebugView = preview ? 4u : 0u;
    HelpMarker("显示空间解包前的压缩模型输入，并缩放到填满屏幕。"
               "这与“压缩模型输入”调试视图相同。"
               "未启用压缩时显示普通模型输入；必须开启应用模型。");
    if (!config->DlssNrSpatialCompression.value_or_default())
        return;

    const auto feature = State::Instance().currentFeature;
    const bool nativeVk = feature && feature->Api() == API::Vulkan && !feature->IsWithDx12();
    const auto status = ReadStatus(nativeVk ? Backend::Vulkan : Backend::Dx12);
    if (!status.spatialStatus.empty())
    {
        const auto localizedStatus = LocalizeStatus(status.spatialStatus);
        ImGui::TextWrapped("%s", localizedStatus.c_str());
    }

    static Spatial::Settings pending;
    static bool editing = false;
    if (!editing)
        pending = Spatial::ReadSettings(*config);
    const float scale = config->DlssNrWorkingScale.value_or_default();
    ConstrainSpatialControls(pending, scale);
    bool commit = false;
    auto slider = [&](const char* label, float& value, float lo, float hi)
    {
        if (ImGui::SliderFloat(label, &value, lo, hi, "%.1f%%"))
            editing = true;
        if (ImGui::IsItemDeactivatedAfterEdit())
            commit = true;
    };
    slider("中心宽度", pending.centerX, 1.0f, pending.workX - 0.5f);
    slider("中心高度", pending.centerY, 1.0f, pending.workY - 0.5f);
    const float minimumWork = Spatial::MinimumWorkPercent(scale);
    slider("工作宽度", pending.workX, std::max(minimumWork, pending.centerX + 0.5f), 100.0f);
    slider("工作高度", pending.workY, std::max(minimumWork, pending.centerY + 0.5f), 100.0f);
    const float xLimit = Spatial::MaxCenterOffset(pending.centerX);
    const float yLimit = Spatial::MaxCenterOffset(pending.centerY);
    slider("中心水平偏移", pending.offsetX, -xLimit, xLimit);
    slider("中心垂直偏移", pending.offsetY, -yLimit, yLimit);
    const auto xShift = Spatial::WorkShiftLimits(pending, false);
    const auto yShift = Spatial::WorkShiftLimits(pending, true);
    slider("工作区域水平偏移", pending.shiftX, xShift.first, xShift.second);
    slider("工作区域垂直偏移", pending.shiftY, yShift.first, yShift.second);
    HelpMarker("偏移过大可能使边缘少于一个工作像素，压缩会回退到普通 NR；上方状态会说明原因。");
    if (ImGui::SmallButton("重置压缩布局"))
    {
        pending = Spatial::Settings {};
        commit = true;
    }
    if (commit)
    {
        ConstrainSpatialControls(pending, scale);
        StoreSpatial(config, pending);
        editing = false;
    }
    Checkbox("显示中心轮廓", config->DlssNrSpatialShowCenter);
    Checkbox("显示工作区域轮廓", config->DlssNrSpatialShowWork);
    ImGui::TextWrapped("中心细节会跟随模型分辨率。强烈的边缘压缩可能使细节变软，或在移动时产生闪烁。");
}

void RenderInput(Config* config)
{
    // Resolution changes rebuild model resources; commit only after releasing the slider.
    static int pendingScale = -1;

    int scalePercent =
        pendingScale >= 0 ? pendingScale : (int) lroundf(config->DlssNrWorkingScale.value_or_default() * 100.0f);

    if (ImGui::SliderInt("模型分辨率", &scalePercent, 25, 200, "%d%%"))
        pendingScale = scalePercent;

    if (ImGui::IsItemDeactivatedAfterEdit() && pendingScale >= 0)
    {
        config->DlssNrWorkingScale = std::clamp(pendingScale, 25, 200) / 100.0f;
        if (config->DlssNrSpatialCompression.value_or_default())
        {
            auto spatial = Spatial::ReadSettings(*config);
            ConstrainSpatialControls(spatial, config->DlssNrWorkingScale.value_or_default());
            StoreSpatial(config, spatial);
        }
        pendingScale = -1;
    }

    HelpMarker("50% 会将宽高都减半；100% 使用完整输入尺寸。");
    RenderSpatial(config);

    if (scalePercent > 100)
    {
        static const char* dsNames[] = { "FSR1",     "双三次", "Catmull-Rom", "Lanczos2",
                                         "Lanczos3", "Kaiser2", "Kaiser3",     "MAGIC" };
        int ds = (int) config->DlssNrScalingDownscaler.value_or_default();
        if (ds < 0 || ds >= IM_ARRAYSIZE(dsNames))
            ds = (int) Scaler::Lanczos3;

        if (ImGui::Combo("降采样器（NR）", &ds, dsNames, IM_ARRAYSIZE(dsNames)))
            config->DlssNrScalingDownscaler = (Scaler) ds;

        HelpMarker("用于超过 100% 分辨率时的降采样滤镜。");
    }
    {
        const bool reduced = config->DlssNrWorkingScale.value_or_default() < 0.999f;

        ImGui::BeginDisabled(!reduced);

        static const char* enlargeNames[] = {
            "经典",           "匹配残差",         "匹配残差 + DLSS",
            "光照 + 色彩", "光照 + 色彩 + DLSS", "边缘感知"
        };
        int enlarge = (int) std::min(config->DlssNrTransfer.value_or_default(), 5u);

        if (ImGui::Combo("放大方式", &enlarge, enlargeNames, IM_ARRAYSIZE(enlargeNames)))
            config->DlssNrTransfer = (uint32_t) enlarge;

        ImGui::EndDisabled();

        HelpMarker("低于 100% 时，分别调整光照增益和色彩变化，再以完整分辨率应用。"
                   "这可以减少放大光晕；纯黑像素保持纯黑。"
                   "边缘感知会参考原图亮度，让编辑落在边缘正确一侧。"
                   "DLSS 模式需要放大后的 DX12 处理。");
    }
    static const char* reversibleNames[] = { "关闭（柔和拐点）", "Neutwo 代理 + 合成", "Neutwo 代理 + 替换",
                                             "混合代理 + 合成", "混合代理 + 替换" };
    int reversible = (int) config->DlssNrReversibleMode.value_or_default();
    if (reversible < 0 || reversible > 4)
        reversible = 0;
    if (ImGui::Combo("HDR 映射（实验）", &reversible, reversibleNames, IM_ARRAYSIZE(reversibleNames)))
        config->DlssNrReversibleMode = (uint32_t) reversible;

    HelpMarker("HDR 映射曲线。替换模式会绕过强度和高光控制。");

    if (reversible == 2 || reversible == 4)
        Slider("恢复锐度", config->DlssNrReplaceDetailStrength, 0.0f, 2.0f, "%.2f", 0.0f);

    const char* exposureNames[] = { "手动", "游戏曝光", "自动 HDR 曝光" };
    const auto source = config->DlssNrWhitePointSource.value_or_default();
    int selected = source == 3 ? 2 : source == 1 ? 1 : 0;
    if (ImGui::Combo("白点来源", &selected, exposureNames, 3))
        config->DlssNrWhitePointSource = selected == 2 ? 3u : static_cast<unsigned>(selected);
    if (selected)
    {
        auto& trim = selected == 2 ? config->DlssNrAutoExposureTrim : config->DlssNrWhitePointTrim;
        Slider("曝光微调", trim, 0.001f, 1000.0f, "%.3fx", selected == 2 ? 5.0f : 1.0f,
               ImGuiSliderFlags_Logarithmic);
        if (selected == 2)
            Slider("高光保护", config->DlssNrAutoExposureHighlightProtection, 0.0f, 100.0f, "%.0f%%", 0.0f);
        auto& curve = selected == 2 ? config->DlssNrAutoExposureTrimAnchors : config->DlssNrExposureTrimAnchors;
        char text[512] {};
        const auto value = curve.value_or_default();
        std::memcpy(text, value.data(), std::min(value.size(), sizeof(text) - 1));
        if (ImGui::InputText("微调锚点", text, sizeof(text)))
            curve = std::string(text);
        HelpMarker("最多八组基础白点:微调值，例如 1:5 100:2。微调按对数插值。清空可使用固定微调值。");
    }
    Slider("纸白", config->DlssNrWhitePointScale, 0.25f, 2000.0f, "%.2fx", {}, ImGuiSliderFlags_Logarithmic);
    HelpMarker("数值越高，NR 输入越暗；数值越低，输入越亮。");
}

// Model tuning rebuilds the feature; commit slider changes only on release.
template <typename Option>
static void DeferredSlider(const char* label, Option* opt, float mn, float mx, float def, bool inheritReset = false)
{
    static std::unordered_map<ImGuiID, float> pending;
    const ImGuiID id = ImGui::GetID(label);

    auto it = pending.find(id);
    float value = it != pending.end() ? it->second : (opt->value_or(def));

    if (ImGui::SliderFloat(label, &value, mn, mx, "%.2f"))
        pending[id] = value;

    if (ImGui::IsItemDeactivatedAfterEdit())
    {
        *opt = std::clamp(value, mn, mx);
        pending.erase(id);
    }

    ImGui::SameLine();

    const std::string resetId = std::string("重置##") + label;
    if (ImGui::SmallButton(resetId.c_str()))
    {
        if (inheritReset)
            *opt = std::optional<float> {};
        else
            *opt = def;
        pending.erase(id);
    }

    if (std::strcmp(label, "强度") == 0)
        HelpMarker("增强强度，1 为默认值。");
    else if (std::strcmp(label, "局部结构") == 0)
        HelpMarker("细节和局部对比度，1 为默认值。");
    else if (std::strcmp(label, "局部色调") == 0)
        HelpMarker("整体光照变化，后续层默认值为 0。");
    else if (std::strcmp(label, "皮肤结构") == 0)
        HelpMarker("皮肤细节，-1 跟随局部结构。");
}

template <typename Option> static void NrModelCombo(Option* opt, bool allowInherit)
{
    static const char* names[] = { "自动", "原始", "优化" };
    const bool inherited = allowInherit && !opt->has_value();
    const uint32_t preset = opt->has_value() ? opt->value() : 0u;
    const char* preview = inherited                      ? "继承第 1 层"
                          : preset < IM_ARRAYSIZE(names) ? names[preset]
                                                         : "不可用的预设";

    if (ImGui::BeginCombo("NR 模型", preview))
    {
        if (allowInherit && ImGui::Selectable("继承第 1 层", inherited))
            *opt = std::optional<uint32_t> {};
        for (uint32_t value = 0; value < IM_ARRAYSIZE(names); ++value)
        {
            const bool selected = !inherited && preset == value;
            if (ImGui::Selectable(names[value], selected))
                *opt = value;
            if (selected)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    HelpMarker("自动：使用已安装 NR 运行时的默认模型。\n"
               "原始：使用原始模型（预设 1）。\n"
               "优化：使用更小的 FP8 模型（预设 2），包含在 NR 310.9.16.0 中。\n"
               "旧版运行时若模型不可用，会回退到默认模型。\n"
               "修改模型会重建本层并重置历史。");
}

void RenderModel(Config* config)
{
    bool unlockPasses = config->DlssNrUnlockPasses.value_or_default();
    const int menuPassLimit = unlockPasses ? 10 : 2;
    static int passes = 1;
    static bool editingPasses = false;
    if (!editingPasses)
        passes = (int) std::clamp(config->DlssNrPasses.value_or_default(), 1u, (unsigned) menuPassLimit);
    ImGui::SliderInt("模型层数", &passes, 1, menuPassLimit, "%d", ImGuiSliderFlags_AlwaysClamp);
    editingPasses = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit())
        config->DlssNrPasses = (uint32_t) std::clamp(passes, 1, menuPassLimit);
    if (ImGui::Checkbox("解锁最多 10 层", &unlockPasses))
    {
        config->DlssNrUnlockPasses = unlockPasses;
        config->DlssNrPasses = std::clamp(config->DlssNrPasses.value_or_default(), 1u, unlockPasses ? 10u : 2u);
    }

    static unsigned selectedPass = 0;
    const auto selectedLabel = std::format("第 {} 层", selectedPass + 1);
    if (ImGui::BeginCombo("编辑层", selectedLabel.c_str()))
    {
        for (unsigned pass = 0; pass <= std::size(config->DlssNrPassOverrides); ++pass)
        {
            const auto label = std::format("第 {} 层", pass + 1);
            if (ImGui::Selectable(label.c_str(), selectedPass == pass))
                selectedPass = pass;
            if (selectedPass == pass)
                ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (selectedPass >= config->DlssNrPasses.value_or_default())
        ImGui::TextDisabled("未启用的层。启用该层后设置才会生效。");

    // Distinct widget IDs keep uncommitted slider edits with their selected pass.
    ImGui::PushID((int) selectedPass);
    const bool inherited = selectedPass != 0;
    const auto tuning = [&](auto& intensity, auto& structure, auto& tone, auto& skin, auto& autoMask)
    {
        DeferredSlider("强度", &intensity, 0.0f, 2.0f,
                       inherited ? config->DlssNrIntensity.value_or_default() : 1.0f, inherited);
        DeferredSlider("局部结构", &structure, 0.0f, 2.0f,
                       inherited ? config->DlssNrLocalStructure.value_or_default() : 1.0f, inherited);
        DeferredSlider("局部色调", &tone, 0.0f, 2.0f, inherited ? 0.0f : 1.0f, inherited);
        DeferredSlider("皮肤结构", &skin, -1.0f, 2.0f,
                       inherited ? config->DlssNrSkinStructure.value_or_default() : -1.0f, inherited);
        bool mask = autoMask.value_or(inherited ? config->DlssNrAutoMask.value_or_default() : true);
        if (ImGui::Checkbox("自动皮肤蒙版", &mask))
            autoMask = mask;
        if (inherited)
        {
            ImGui::SameLine();
            if (ImGui::SmallButton("重置##mask"))
                autoMask = std::optional<bool> {};
        }
        HelpMarker("基于模型选择皮肤区域。");
    };
    static const char* styles[] = { "标准", "自然", "电影" };
    static const char* inheritedStyles[] = { "自动", "标准", "自然", "电影" };
    if (selectedPass == 0)
    {
        NrModelCombo(&config->DlssNrPreset, false);
        int style = (int) std::min(config->DlssNrStyle.value_or_default(), 2u);
        if (ImGui::Combo("风格", &style, styles, IM_ARRAYSIZE(styles)))
            config->DlssNrStyle = (uint32_t) style;
        tuning(config->DlssNrIntensity, config->DlssNrLocalStructure, config->DlssNrLocalTone,
               config->DlssNrSkinStructure, config->DlssNrAutoMask);
    }
    else
    {
        auto& pass = config->DlssNrPassOverrides[selectedPass - 1];
        if (selectedPass <= 2)
            NrModelCombo(&pass.preset, true);
        else
            ImGui::TextDisabled("NR 模型：继承第 1 层");
        int style = pass.style.has_value() ? std::clamp((int) pass.style.value(), 0, 2) + 1 : 0;
        if (ImGui::Combo("风格", &style, inheritedStyles, IM_ARRAYSIZE(inheritedStyles)))
            pass.style = style ? std::optional<uint32_t>(style - 1) : std::nullopt;
        tuning(pass.intensity, pass.structure, pass.tone, pass.skin, pass.autoMask);
    }
    ImGui::PopID();
}

void RenderBlend(Config* config)
{
    if (config->DlssNrResidualAcrossRr.value_or_default())
    {
        Checkbox("拒绝过期历史", config->DlssNrHistoryRejection);
        HelpMarker("RR 残差路径：输入变化时拒绝旧编辑，并限制到当前编辑附近，可减少拖影。关闭则保留原历史滤镜。");
        Slider("历史置信度阈值", config->DlssNrResidualConfidenceSensitivity, 0.0f, 2.0f, "%.3f", 0.0f);
    }
    if (ImGui::TreeNode("光晕抑制（实验）"))
    {
        Slider("亮物体附近暗边", config->DlssNrHaloSuppression, 0.0f, 1.0f, "%.2f", 0.0f);
        Slider("暗物体附近暗边", config->DlssNrHaloBrightSuppression, 0.0f, 1.0f, "%.2f", 0.0f);
        Slider("光晕半径（NR 像素）", config->DlssNrHaloRadius, 1.0f, 4.0f, "%.0f", 2.0f);
        HelpMarker("提亮细小暗边，同时保留整体光照。两项强度都为 0 时停用。可能会提亮真实阴影，请对比后再保留。");
        ImGui::TreePop();
    }
    if (config->DlssNrFinishedPicture.value_or_default() &&
        (config->DlssNrRunBeforeSr.value_or_default() || config->DlssNrDeferredDlss.value_or_default()))
    {
        const auto feature = State::Instance().currentFeature;
        ImGui::BeginDisabled(State::Instance().swapchainApi == API::Vulkan ||
                             (feature && feature->GetUpscalerType() == Upscaler::DLSSD));
        Checkbox("匹配 HDR 亮度响应（实验）", config->DlssNrHdrTransfer);
        ImGui::EndDisabled();
        HelpMarker(
            "将早期 NR 的亮度变化匹配到最终 HDR 图像，会增加 GPU 工作量；不可靠时会回退。");
    }
    Slider("细节强度", config->DlssNrTransferStrength, 0.0f, 2.0f, "%.2f", 1.0f);
    HelpMarker("0 = 不改变细节；1 = 正常。");

    Slider("色彩强度", config->DlssNrColourStrength, 0.0f, 4.0f, "%.2f", 1.0f);
    HelpMarker("0 = 游戏色彩；1 = 模型色彩；超过 1 会增强饱和度。");

    if (ImGui::TreeNode("皮肤与环境（最终编辑）"))
    {
        Checkbox("分离皮肤/环境控制", config->DlssNrSkinProtection);
        ImGui::BeginDisabled(!config->DlssNrSkinProtection.value_or_default());
        const auto slider = [](const char* label, auto& option)
        {
            Slider(label, option, 0.0f, 1.0f);
            HelpMarker("0 = 不改变；1 = 完整效果。");
        };
        slider("皮肤细节/光照", config->DlssNrSkinDetail);
        slider("皮肤色彩", config->DlssNrSkinColour);
        slider("环境细节/光照", config->DlssNrEnvironmentDetail);
        slider("环境色彩", config->DlssNrEnvironmentColour);
        Checkbox("预览基于色彩的蒙版", config->DlssNrShowSkinMask);
        ImGui::EndDisabled();
        ImGui::TreePop();
    }

    Slider("高光保护", config->DlssNrMaxRatio, 1.0f, 8.0f, "%.1fx", 2.0f);
    HelpMarker("限制像素的变亮和变暗幅度。");
}

void RenderInspect(Config* config)
{
    const auto placement = DlssNr::ResolvePlacement(
        config->DlssNrRunBeforeSr.value_or_default(), config->DlssNrDeferredDlss.value_or_default(),
        config->DlssNrResidualAcrossRr.value_or_default(), config->DlssNrFinishedPicture.value_or_default());
    if (placement.deferred && (config->DlssNrCompare.value_or_default() || config->DlssNrDebugView.value_or_default() ||
                               config->DlssNrShowSkinMask.value_or_default()))
        ImGui::TextWrapped("对比、调试视图和皮肤蒙版检查会暂停独立编辑放大路径。");
    Checkbox("保持帧", config->DlssNrHoldFrame);
    HelpMarker(
        "冻结一帧以调整 NR。之后的游戏效果可能继续更新，因此时序表现不具代表性。");

    static const char* compareNames[] = { "关闭", "并排对比", "擦除对比" };
    int compare = (int) config->DlssNrCompare.value_or_default();
    if (ImGui::Combo("对比", &compare, compareNames, IM_ARRAYSIZE(compareNames)))
        config->DlssNrCompare = (uint32_t) compare;

    HelpMarker("对比原图与 NR 输出。");

    if (compare != 0)
    {
        Checkbox("交换两侧", config->DlssNrCompareSwap);
        Checkbox("标注两侧", config->DlssNrCompareTags);
        if (config->DlssNrCompareTags.value_or_default())
            Slider("标签大小", config->DlssNrTagScale, 0.5f, 5.0f, "%.1fx", {}, ImGuiSliderFlags_AlwaysClamp);
    }

    if (compare == 1)
    {
        Slider("缩放", config->DlssNrCompareZoom, 1.0f, 2.0f, "%.2f", {}, ImGuiSliderFlags_AlwaysClamp);
        HelpMarker("1 = 适应；2 = 裁剪并放大。");
    }

    if (compare == 2)
    {
        Slider("分界线", config->DlssNrCompareSplit, 0.0f, 1.0f, "%.2f", {}, ImGuiSliderFlags_AlwaysClamp);
        HelpMarker("移动对比分界线。");
    }

    static const char* debugNames[] = { "关闭", "代理图（模型看到的内容）", "模型输出（原始）",
                                        "差异（增强）", "压缩模型输入" };
    int debugView = (int) config->DlssNrDebugView.value_or_default();
    if (ImGui::Combo("调试视图", &debugView, debugNames, IM_ARRAYSIZE(debugNames)))
        config->DlssNrDebugView = (uint32_t) debugView;

    HelpMarker("差异会增强 20 倍；灰色表示未改变。代理图和原始模型输出使用解包后的几何。"
               "压缩模型输入显示解包前的输入，并缩放到填满屏幕。");
}
} // namespace DlssNr::MenuSections
