# Native Vulkan DLSSG with pre-SR NR

This compatibility path keeps the game's own Vulkan Streamline DLSSG integration. It does not implement an OptiScaler-generated Vulkan FG output or add frame generation to a game without native DLSSG.

For a game using Streamline 2 and declaring `sl::RenderAPI::eVulkan`, OptiScaler preserves the requested DLSS-G plugin, its Vulkan requirements and plugin metadata, and forwards capability queries and feature functions to the real runtime. Vulkan resources, constants and frame markers stay out of the D3D12 FG input collector. DLSSG options and state remain owned by the game.

The native route requires no Nukem/Enabler/FFX FG replacement to be selected. Existing replacement configurations keep their original route and are not mislabeled as native DLSSG. To return to the native route, set OptiScaler FG Input and Output to None, save, and restart. Native Vulkan NGX capabilities are not overwritten with replacement FG availability or a fabricated maximum frame count.

With `[DlssNr] RunBeforeSR=true`, the rendering sequence stays:

```text
game render-resolution colour
  -> OptiScaler NR
  -> game's DLSS Super Resolution
  -> game's post-processing / native DLSSG integration
  -> presentation
```

The game's placement of HUD and post-processing remains game-specific. This change does not run NR a second time on generated frames.

## Controls

Enable DLSS Frame Generation in the game's settings. The OptiScaler menu reports the game's request under **Frame Generation (Game native)**. The separate OptiScaler FG Input/Output replacement controls are not used for this path.

The existing Vulkan menu interlock is retained: opening the OptiScaler menu pauses DLSSG while retaining resources; closing it restores the game's original request, including Auto mode. Replay occurs on the Vulkan present thread, not from a UI control callback. A failed pause must not permit overlay writes to a potentially busy swapchain image. Unknown options versions or caller-owned extension chains are not cached for later replay.

With the main menu closed, FPS and notification overlays can remain visible without pausing native FG. This is limited to recognized native WSI paths: before installing hooks, GetSwapchainImagesKHR must resolve to nvoglv64.dll; CreateSwapchainKHR and QueuePresentKHR must belong either to that same driver module or to the same OBS graphics-hook64.dll module exposing OBS_Negotiate. The overlay obtains images through that verified query function and checks the swapchain, image index and registered graphics queue before drawing. Unsupported layers or queues are skipped instead of treating Streamline's offscreen input images as output images.

The overlay waits every semaphore from the current VkPresentInfoKHR at ALL_COMMANDS, submits on the actual presenting graphics queue, then replaces the present waits with its own completion semaphore for that swapchain image. It does not query DLSSG state again, cast its opaque input-completion fence, or run NR on generated frames. Unknown options and extension chains remain untouched: passive overlays at the verified output boundary are allowed, but an interactive menu is withheld if the original options cannot be safely paused and restored.

Passive overlays bypass the legacy menu delay based on the verified WSI boundary, even if OptiScaler loaded too late to observe slInit. The native Streamline route flag is not required for this output-only draw. The log records the first delay bypass and successful passive submission at Info level; the main menu still obeys its delay and FG interlock.

The boundary follows NVIDIA's [Streamline manual Vulkan hooking guide, section 4.2](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md#42-vulkan), which separates native Vulkan entry points from the swapchain/present proxies exported by sl.interposer.dll. See also the [DLSSG Vulkan synchronization and swapchain guidance](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideDLSS_G.md). The supported path does not claim compatibility with arbitrary third-party Vulkan layers or inline hooks.

The recognized [OBS Vulkan capture layer](https://github.com/obsproject/obs-studio/blob/343ae6015a976a7518d5f5635080ea9add69a65d/plugins/win-capture/graphics-hook/vulkan-capture.c) forwards native swapchain handles and does not replace GetSwapchainImagesKHR. Its present wrapper captures on the presenting queue and forwards the same present information. A local RTX 4080 SUPER device probe found this OBS/NVIDIA entry-point combination; that probe validates module routing, not Endfield or DLSSG rendering.

The menu reports API results rather than claiming frame generation is active just because an option was selected. Actual frame synthesis still depends on the game's integration and the NVIDIA runtime accepting its requests.

## Validation and game test

The regression runner `tests/run_native_vulkan_dlssg.ps1` exercises production hook bodies with controlled Streamline entry points. `tests/run_vulkan_overlay.ps1` extracts the production present submission and ImGui queue selection functions to check wait propagation, image/queue binding, invalid targets and failure handling. These tests are separate from an in-game test and do not load NVIDIA's model runtime.

For Endfield, preserve the working NR DLL/model configuration, replace the loaded OptiScaler proxy DLL, enable native DLSS frame generation in the game, and enable NR manually in OptiScaler. Enable the FPS overlay, close the main menu and check that the counter remains visible with FG requested. Compare with an external counter rather than assuming OptiScaler's counter measures generated frames. Check notifications, repeated menu open/close cycles, FG toggles, resize/Alt-Tab and transitions into/out of gameplay; retain `OptiScaler.log` for failures.

Previously supplied screenshots validate NR with Vulkan + DLSS SR on an RTX 4080 SUPER. They do not validate this later native-FG compatibility change. Results for this change must be recorded separately.
