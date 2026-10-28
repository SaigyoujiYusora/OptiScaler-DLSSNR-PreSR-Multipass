# v10.0.0-dev 中文菜单 + Vulkan 浮窗修复

- 改动：菜单和 NR 设置汉化了，不用再对着英文调参数了😋
- 修复：改善原生 Vulkan 下 FPS／通知浮窗的兼容。
- 更新：合并上游 NR 边缘／历史／光晕过滤，以及 40 系 MFG 运行时重新加载的兼容修复。

安装：首次使用解压全部文件到游戏 EXE 目录，运行 `setup_windows.bat` 然后选择 dll 名称（终末地选 `d3d12.dll`）。已经装过的退出游戏后替换对应 DLL，保留自己的 `OptiScaler.ini`。

40 系多帧生成还是用带 MFG 补丁的版本。这次的兼容修复不代表 Vulkan 下可以在 OptiScaler 里直接改插帧倍率，原生 FG 还是在游戏里开。

NVIDIA NR 模型 DLL 不属于项目 GPL 授权范围。
