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

With the main menu closed, FPS and notification overlays can remain visible without pausing native FG.
The overlay binds a complete per-device WSI dispatch obtained from the original Vulkan loader resolver.
Streamline's input swapchain proxies come from its separate interposer resolver; they are excluded.
Ordinary transparent layers in the native loader chain are supported without DLL names, hashes or version lists.

Device registration captures CreateSwapchain, DestroySwapchain, QueuePresent and GetSwapchainImages together.
Hook installation uses that exact snapshot rather than resolving a second set. A device becomes bound only
after the hook transaction succeeds. A later device sharing those hook targets can bind; different targets
are rejected rather than routed through the first device's global trampolines. Known Streamline interposer
or plugin entry points, including recognized detour targets, are rejected.

Only a successful raw CreateSwapchain call records a swapchain with its device and surface. The image list
is enumerated through the same device dispatch and confirmed nonempty, nonnull and unique before passive
drawing is enabled. Present must use the recorded swapchain, its actual graphics/present queue and family,
and a valid image index. The overlay waits all incoming semaphores at ALL_COMMANDS, then forwards its own
completion semaphore while preserving pNext and pResults. Submit failures disable drawing until recreation.

The original loader resolver and its Detours target are tied to the loaded vulkan-1 module. This identifies
the native loader chain below normal Streamline WSI proxies, not an arbitrary third-party virtual-swapchain
implementation. Surface/size/image self-consistency alone is not treated as evidence of an FG output.
No upper-layer opaque handle is passed directly to an ICD for speculative validation.

The legacy menu-delay bypass is based on this bound native chain and closed-menu state, independently of
whether slInit was observed. Interactive menus still pause native FG; closing restores the game's request.
See NVIDIA's [manual Vulkan hooking guide, section 4.2](https://github.com/NVIDIA-RTX/Streamline/blob/main/docs/ProgrammingGuideManualHooking.md#42-vulkan)
for the distinction between native loader entries and Streamline input-swapchain proxies.

The menu reports API results rather than claiming frame generation is active just because an option was selected. Actual frame synthesis still depends on the game's integration and the NVIDIA runtime accepting its requests.

## Validation and game test

The earlier No Man's Sky workaround recognized one OBS/Steam DLL combination. It has been replaced by
native loader dispatch provenance and object lifetime validation; no Steam version pin remains.
The tests in `run_vulkan_overlay.ps1` exercise the production dispatch helper, registry and hook-binding
functions with anonymous transparent callbacks, proxy contamination, failed transactions, multiple devices,
raw versus unregistered proxy swapchains, invalid image arrays, wrong queues, destruction and presentation.
They also preserve existing image-index/semaphore forwarding and submit-failure cases.

The regression runner `tests/run_native_vulkan_dlssg.ps1` exercises production hook bodies with controlled Streamline entry points. `tests/run_vulkan_overlay.ps1` extracts the production present submission and ImGui queue selection functions to check wait propagation, image/queue binding, invalid targets and failure handling. These tests are separate from an in-game test and do not load NVIDIA's model runtime.

For Endfield, preserve the working NR DLL/model configuration, replace the loaded OptiScaler proxy DLL, enable native DLSS frame generation in the game, and enable NR manually in OptiScaler. Enable the FPS overlay, close the main menu and check that the counter remains visible with FG requested. Compare with an external counter rather than assuming OptiScaler's counter measures generated frames. Check notifications, repeated menu open/close cycles, FG toggles, resize/Alt-Tab and transitions into/out of gameplay; retain `OptiScaler.log` for failures.

Previously supplied screenshots validate NR with Vulkan + DLSS SR on an RTX 4080 SUPER. They do not validate this later native-FG compatibility change. Results for this change must be recorded separately.
