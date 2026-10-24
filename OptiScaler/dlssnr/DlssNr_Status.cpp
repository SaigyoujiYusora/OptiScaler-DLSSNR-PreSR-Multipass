#include <pch.h>
#include "DlssNr_Status.h"

#include <array>
#include <string>
#include <mutex>

namespace DlssNr
{
namespace
{
struct PublishedStatus
{
    const void* owner = nullptr; // Identity only; never dereferenced.
    StatusSnapshot value;
};

std::mutex statusMutex;
std::array<PublishedStatus, 2> published;
ControlRequests requests;
} // namespace

StatusSnapshot ReadStatus(Backend backend)
{
    std::lock_guard lock(statusMutex);
    return published[static_cast<size_t>(backend)].value;
}

void PublishStatus(const void* owner, Backend backend, const StatusSnapshot& status)
{
    std::lock_guard lock(statusMutex);
    published[static_cast<size_t>(backend)] = { owner, status };
}

void ClearStatus(const void* owner)
{
    std::lock_guard lock(statusMutex);
    for (auto& status : published)
        if (status.owner == owner)
            status = {};
}

ControlRequests ReadControlRequests()
{
    std::lock_guard lock(statusMutex);
    return requests;
}

void RetryAfterFailure()
{
    std::lock_guard lock(statusMutex);
    ++requests.retryGeneration;
}

void RequestCapture(unsigned int frames)
{
    std::lock_guard lock(statusMutex);
    requests.captureFrames = frames;
    ++requests.captureGeneration;
}

std::optional<double> LastGpuTime() { return ReadStatus(Backend::Dx12).gpuTime; }

std::string LocalizeStatus(std::string_view text)
{
    static constexpr std::pair<std::string_view, std::string_view> exact[] {
        std::pair { "off", "关闭" },
        { "not started", "尚未开始" },
        { "Waiting for a finished picture.", "等待最终画面。" },
        { "Waiting for a finished Vulkan picture.", "等待最终 Vulkan 画面。" },
        { "Waiting for Vulkan depth and movement data.", "等待 Vulkan 深度和运动数据。" },
        { "Waiting for a tracked primary Vulkan command buffer.", "等待已跟踪的主 Vulkan 命令缓冲区。" },
        { "Enable NR before creating the Vulkan swapchain; restart the game if NR was enabled during play.",
          "请在创建 Vulkan 交换链前启用 NR；如果游戏中途才启用 NR，请重启游戏。" },
        { "The Vulkan swapchain does not support finished-picture transfers.", "Vulkan 交换链不支持最终画面传输。" },
        { "This Vulkan screen colour space is not supported.", "不支持当前 Vulkan 屏幕色彩空间。" },
        { "Vulkan finished-picture submission failed.", "Vulkan 最终画面提交失败。" },
        { "Applying NR to the finished Vulkan picture.", "正在将 NR 应用到最终 Vulkan 画面。" },
        { "Preparing NR for the finished Vulkan picture.", "正在为最终 Vulkan 画面准备 NR。" },
        { "Native Vulkan finished-picture NR runs at presentation; disable Generate model before upscale and Generate before SR, apply after SR.",
          "原生 Vulkan 最终画面 NR 在呈现阶段运行；请关闭“放大前生成模型”和“放大前生成，放大后应用”，并启用“放大后应用”。" },
        { "the NVIDIA NGX Vulkan driver could not initialize", "NVIDIA NGX Vulkan 驱动初始化失败" },
        { "the NVIDIA NGX Vulkan driver interface is incomplete", "NVIDIA NGX Vulkan 驱动接口不完整" },
        { "the NVIDIA NGX driver could not allocate capability parameters", "NVIDIA NGX 驱动无法分配能力参数" },
        { "the NR backend reused a feature instead of creating an independent NR pass", "NR 后端复用了已有特性，而不是创建独立的 NR 层" },
        { "Neural Rendering feature creation failed; see the Vulkan backend result in the log", "神经渲染特性创建失败；请查看日志中的 Vulkan 后端结果" },
        { "the Vulkan device could not retire work for retry", "Vulkan 设备无法回收重试所需的工作" },
        { "the Vulkan device could not retire previous model resources", "Vulkan 设备无法回收之前的模型资源" },
        { "the pass could not allocate its own surfaces", "本层无法分配自己的纹理" },
        { "could not allocate the model creation marker", "无法分配模型创建标记" },
        { "the encode dispatch failed", "编码派发失败" },
        { "the downsample dispatch failed", "降采样派发失败" },
        { "the model refused to evaluate", "模型拒绝执行评估" },
        { "the resolve dispatch failed", "合成派发失败" },
        { "Waiting for the DirectX 12 bridge commands.", "等待 DirectX 12 桥接命令。" },
        { "The graphics device changed. Restart the game to use this option.", "图形设备已改变。请重启游戏后使用此选项。" },
        { "Waiting for the previous picture to finish.", "等待上一帧画面完成。" },
        { "Could not prepare the finished-picture option.", "无法准备最终画面选项。" },
        { "The game's depth or movement data is not supported.", "不支持游戏提供的深度或运动数据。" },
        { "The upscaled changes could not be saved.", "无法保存放大后的变化。" },
        { "This screen colour format is not supported.", "不支持当前屏幕色彩格式。" },
        { "Could not hold the finished frame.", "无法保持最终画面。" },
        { "Could not prepare the finished picture.", "无法准备最终画面。" },
        { "Could not finish the picture. Restart the game to retry.", "无法完成画面处理。请重启游戏后重试。" },
        { "The graphics queue stopped. Restart the game to retry.", "图形队列已停止。请重启游戏后重试。" },
        { "NR changes are hidden.", "NR 变化已隐藏。" },
        { "Disable the Debug view, Compare and Show skin mask first.", "请先关闭调试视图、对比和显示皮肤蒙版。" },
        { "inactive: requires a direct command list with restorable game state", "未激活：需要可恢复游戏状态的直接命令列表" },
        { "inactive: distinct Color/Output, depth and motion are required", "未激活：需要独立的色彩/输出、深度和运动资源" },
        { "inactive: non-zero colour/guide/output offsets", "未激活：色彩/引导图/输出存在非零偏移" },
        { "inactive: unsupported active input/output dimensions", "未激活：不支持当前有效输入/输出尺寸" },
        { "waiting for retired GPU work", "等待 GPU 工作回收" },
        { "allocation failed", "分配失败" },
        { "private exposure initialization failed", "独立曝光初始化失败" },
        { "private DLSS SR creation failed", "独立 DLSS SR 创建失败" },
        { "private DLSS SR evaluation failed", "独立 DLSS SR 评估失败" },
        { "edit composition failed; the game's upscale keeps its raw input", "编辑合成失败；游戏放大器继续使用原始输入" },
        { "waiting for NR evaluation; the game's upscale keeps its raw input", "等待 NR 评估；游戏放大器继续使用原始输入" },
        { "waiting to save the upscaled changes for the finished picture", "等待保存最终画面的放大变化" },
        { "composition failed; clean frame retained", "合成失败；保留干净画面" },
        { "waiting for a same-device direct queue identity", "等待同设备的直接队列标识" },
        { "waiting for retired GPU work; clean SR frame retained", "等待 GPU 工作回收；保留干净 SR 画面" },
        { "waiting for GPU completion slots; clean SR frame retained", "等待 GPU 完成槽位；保留干净 SR 画面" },
        { "waiting for RR residual accumulation; clean game frame retained", "等待 RR 残差累积；保留干净游戏画面" },
        { "inactive: game state cannot be restored after SR", "未激活：SR 后无法恢复游戏状态" },
        { "waiting for GPU completion slots; clean frame retained", "等待 GPU 完成槽位；保留干净画面" },
        { "inactive: game state cannot be restored after the upscale", "未激活：放大后无法恢复游戏状态" },
        { "inactive: the upscaler output changed shape under the private upscale", "未激活：独立放大期间放大器输出尺寸发生变化" },
        { "Spatial compression unavailable: Vulkan shader pipeline failed", "空间压缩不可用：Vulkan 着色器管线失败" },
        { "Spatial compression unavailable: colour packing failed", "空间压缩不可用：色彩打包失败" },
        { "Spatial compression unavailable: guide packing failed", "空间压缩不可用：引导图打包失败" },
        { "Spatial compression unavailable: could not allocate packed surfaces", "空间压缩不可用：无法分配压缩纹理" },
        { "Spatial compression unavailable: the driver rejected the packed model", "空间压缩不可用：驱动拒绝压缩模型" },
        { "Spatial compression unavailable: the driver rejected the packed evaluation", "空间压缩不可用：驱动拒绝压缩评估" },
        { "Spatial compression unavailable: answer unpacking failed", "空间压缩不可用：答案解包失败" },
        { "Spatial compression unavailable: downsampling filter failed", "空间压缩不可用：降采样滤镜失败" },
        { "Spatial compression unavailable: matched downsampling failed", "空间压缩不可用：匹配降采样失败" },
    };
    for (const auto& [source, translated] : exact)
        if (text == source)
            return std::string(translated);

    if (text.starts_with("Spatial compression "))
    {
        std::string result = "空间压缩 ";
        result.append(text.substr(std::string_view("Spatial compression ").size()));
        const auto pixels = result.find(" pixels)");
        if (pixels != std::string::npos)
            result.replace(pixels, 8, " 像素）");
        return result;
    }
    if (text.starts_with("private ") && text.find(" creation failed: ") != std::string_view::npos)
    {
        const auto separator = text.find(" creation failed: ");
        return "独立 " + std::string(text.substr(8, separator - 8)) + " 创建失败：" +
               std::string(text.substr(separator + std::string_view(" creation failed: ").size()));
    }
    if (text.starts_with("private ") && text.find(" evaluation failed: ") != std::string_view::npos)
    {
        const auto separator = text.find(" evaluation failed: ");
        return "独立 " + std::string(text.substr(8, separator - 8)) + " 评估失败：" +
               std::string(text.substr(separator + std::string_view(" evaluation failed: ").size()));
    }
    if (text.starts_with("1:1 ") && text.find(" creation failed: ") != std::string_view::npos)
    {
        const auto separator = text.find(" creation failed: ");
        return "1:1 " + std::string(text.substr(4, separator - 4)) + " 创建失败：" +
               std::string(text.substr(separator + std::string_view(" creation failed: ").size()));
    }
    if (text.starts_with("1:1 ") && text.find(" evaluation failed: ") != std::string_view::npos)
    {
        const auto separator = text.find(" evaluation failed: ");
        return "1:1 " + std::string(text.substr(4, separator - 4)) + " 评估失败：" +
               std::string(text.substr(separator + std::string_view(" evaluation failed: ").size()));
    }

    // Preserve unknown diagnostic detail while making its UI role explicit.
    return std::string("状态：") + std::string(text);
}
} // namespace DlssNr
