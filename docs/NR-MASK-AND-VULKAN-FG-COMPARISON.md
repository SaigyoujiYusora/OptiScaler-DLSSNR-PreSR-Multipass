# Vulkan 帧生成与 Auto Skin Mask 对照

本次以 MP v0.8.91 加本仓库两个修复提交为基线，结合用户在《明日方舟：终末地》中的实测截图做静态对照。截图已证明 RTX 4080 SUPER、原生 Vulkan + DLSS 310.5.2 下 NR 成功运行；以下未把画面问题冒充为已修复。

## 1. 帧生成的 Unsupported API

`OptiScaler/menu/menu_common.cpp:3436`、`:3449`、`:3453` 分别在 `swapchainApi == API::Vulkan` 时禁用 DLSSG、FSR FG、XeFG 输出。`hooks/FG_Hooks.cpp:138`、`:244` 要求 D3D12 command queue，实际输出后端和交换链实现均为 D3D12/DXGI。

因此这是 OptiScaler 自身 FG 输出尚无原生 Vulkan 实现，不是缺少 NR DLL，也不能通过删除菜单禁用条件解决。切换 upscaler 的 `w/Dx12` 桥仍使用 Vulkan 交换链，不会解锁 FG Output。

游戏自带的 DLSS 帧生成需另看：

- `FG Input=None`、`FG Output=None` 时，`Streamline_Hooks.cpp:244` 不会因 OptiScaler 接管 FG 而移除游戏的 DLSSG 插件。
- 选择 `FG Input=DLSSG via Streamline` 表示让 OptiScaler 接管这个输入，不能把它理解成“打开游戏原生 FG”。该条件会进入上面的插件移除分支；在没有可用输出的 Vulkan 路径上，不应这样启用。
- `Streamline_Hooks.cpp:1811` 的菜单互锁会在 Vulkan 的 OptiScaler 菜单显示期间暂时将 DLSSG 设为 Off。测试游戏原生 FG 时应关闭该菜单再判断。

可先将 OptiScaler 的 FG Input/Output 都设为 None，保存并重启，再用游戏自身的帧生成选项测试。此建议以游戏该 API 本身支持 FG 为前提；没有运行游戏验证该组合，也没有声称补齐了 Vulkan FG 后端。

## 2. RenoDX 对照对象及证据范围

用户通过 DLSS5oneclick 安装，未提供具体 consumer 版本。其公开源码 `f4e4260a8a732752440d6b670fe2f7baba7af288`（v0.14.6）的 `src/installer.rs` 表明：安装器可以选择 ShortFuse、RenoDX DLSS5、Neural Upstream 等不同 consumer；RenoDX DLSS5 从 `RankFTW/rhi-repo` 的 `renodx-dlss5-*` 发布下载，并存在 4.55/4.70 回退或固定版本。因此“使用 oneclick”不能唯一确定实际 add-on。

本次实际下载并静态检查的参考件为 [RenoDX DLSS5 6.5.3](https://github.com/RankFTW/rhi-repo/releases/tag/renodx-dlss5-6.5.3)。它是一个可追溯参考版本，不冒充用户当前安装版本。

- ZIP SHA-256：`553B1619B9E5DDFBCB4EBC7F2F3BFFFF9256A48A25B988F4817F5C63F4CAA1DE`，与发布页一致。
- `renodx-dlss5.addon64` SHA-256：`342341F669F1D64E0C70C8593A07A2FAB5075E073DFAE97C331C9A6776260A0A`。
- 仅做 PE 字符串、参数写入调用和默认值的静态检查，没有加载/执行该 DLL。
- 该版本完整源代码未从目前公开的 RenoDX main 获取，`dlss5-anywhere` 分支也不在远端 heads 列表中。下表明确区分可见实现与二进制界面/调用证据，不推断闭源模型的分割算法。

## 3. 两者的实际差异

| 项目 | 当前 OptiScaler MP 修复版 | RenoDX DLSS5 6.5.3 参考件 |
| --- | --- | --- |
| 自动选区的实现归属 | 向模型写入 `DLSSNR.UseAutoMask` | 同样向模型写入 `DLSSNR.UseAutoMask`；界面称由 runtime 自动检测角色 |
| Auto Mask 默认 | `Config.h:280` 为 true | 对应全局初值为 0；恢复默认函数也写入 0 |
| Skin Structure 参数 | `DLSSNR.SkinStructureStrength`，默认 -1 | 同名参数；数据段及恢复默认函数均为 -1 |
| 对 -1 的解释 | 菜单称“跟随 Local structure” | 界面称“负值柔化、正值增强、0 中性” |
| -1 在插件侧是否替换 | `PassProfiles.h` 仅限幅，`SetModelTuning` 原样下发 -1 | 参数写入调用同样直接读取并发送该浮点值 |
| Tone 控制 | 下发 LocalTone；当前公共调参函数未下发 GlobalTone | 可见 LocalTone、GlobalTone 两项参数及控件 |
| 超分前处理 | 原生 Vulkan pre-SR 已被用户截图验证 | 二进制包含 `NRPreUpscale` 及 pre-SR 活动状态/说明；不能简单归为“只在超分后处理” |
| HDR/最终合成 | 可见 soft-knee、Neutwo/hybrid、亮度比限制、色彩混合代码 | 界面暴露 Auto/Classic/Anchored/Display codec、GPU autoscale、色度 stops 限制及 curve-aware transfer；仅凭界面不能宣称每项内部算法优于 MP |

**核心结论：两者的 Auto Mask 都是请求 NR runtime 自动选区，没有证据表明换成 RenoDX 就换了一套独立且更好的皮肤分割模型。它们的默认开关、参数语义提示及图像合成路径确有差异。**

RenoDX 静态调用证据（ImageBase `0x180000000`）：

- `0x1800569B3` 附近为 `DLSSNR.SkinStructureStrength` 写入，读取 `0x1800CD5C0` 的 float；其初值为 -1。
- `0x1800569D2` 至 `0x1800569F1` 为 `DLSSNR.UseAutoMask` 写入，读取 `0x1800CF4B0` 的 bool。
- 恢复默认函数从 `0x180052D60` 开始，在 `0x180052DD7` 写 SkinStructure=-1，在 `0x180052DDF` 写 AutoMask=0；该函数末尾使用 “all settings restored to the built-in defaults” 文本。
- 文件偏移 `0xAA350` 的说明为 “Let the runtime detect characters automatically so the Character/Skin Structure response applies to them.”
- 文件偏移 `0xAA2C0` 的说明为 “Structure response on skin/character regions: negative smooths, positive enhances (0 = neutral).”

## 4. 肤色变暗与白边能确认到哪一步

截图设置为 Cinematic、Intensity=2.00、Local structure=1.25、Local tone=1.00、Skin structure=-1.00、Auto skin mask 开启。单张开启状态截图不能把白边或变暗单独归因于 Auto Mask。

`DlssNr_ModelParameters.h:16` 将 Intensity、Style、LocalStructure、LocalTone、SkinStructure、AutoMask 分别写给模型。关闭 Auto Mask 不是关闭 NR；其余重绘、色调和合成仍然有效。

另外要区分两个蒙版：

- **Auto skin mask** 是模型内的选区开关，本仓库没有得到其内部蒙版图像，也没有在角色轮廓上画白线。
- **Skin and environment / Separate skin controls** 是 MP 另外实现的颜色启发式最终滤镜。`dlssnr.hlsl:56` 的 `SkinColourWeight` 按颜色估计皮肤，`:984` 起衰减最终编辑；它与 Auto Mask 无关，默认关闭。其预览显示的不是 NVIDIA 模型蒙版。

MP 的合成允许亮度改变：`dlssnr.hlsl:948` 把亮度比限制在 `[1/guard, guard]`，不是锁定原始亮度。`:956` 中即使 Colour strength=0，仍然保留 `original * boundedRatio`，所以“色彩强度降到 0”也不保证肤色恢复原亮度。白边还可能来自模型局部对比、proxy 编解码/合成或后续 SR，当前证据不够把某一项判为唯一原因。

两边对负 SkinStructure 的提示相互矛盾，不能把任一提示当成 NVIDIA 已公开确认的语义。本次不据此盲改参数或模型实现。

## 5. 最小同场景 A/B

保持机位、曝光、分辨率、style、intensity、tone 等其他参数一致；每次等待模型重建完成再截取同一角色区域。

| 组别 | Apply model | Auto skin mask | Skin structure | 用途 |
| --- | --- | --- | --- | --- |
| A | 关闭 | 任意 | 任意 | 原始画面对照，隐藏 NR 编辑但模型仍可能运行 |
| B | 开启 | 关闭 | -1 | NR 重绘/合成不依赖自动选区的结果 |
| C | 开启 | 开启 | -1 | 与 B 只差 Auto Mask，直接判断它是否影响白边/肤色 |
| D | 开启 | 开启 | 0 | 检查皮肤结构响应对选区内结果的影响，不预先保证“保护皮肤” |

先完成 B/C 对照，再分别尝试把 Intensity 从 2 降到 1、Local tone 从 1 降到 0 或切换 style，避免同时改多项导致归因失效。尚未取得用户同场景 RenoDX 与 MP 的双份画面，本文是实现/参数对比，不伪造画质优劣对比。

## 6. 仓库状态

本仓库独立保留 MP v0.8.91 历史，已完成并分开提交：

- `8bf9ed28`：Vulkan NR 直接调用回退。
- `6b325dc2`：保存配置时始终写入 `Enabled=auto`。

以上两个提交已按用户明确授权推送到其仓库 main。本文仅记录调查结果，不宣称修复了 FG 输出或模型肤色问题。
