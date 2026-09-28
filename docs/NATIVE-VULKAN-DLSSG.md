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

While native FG is requested and the main menu is closed, OptiScaler's FPS counter and notification overlays are not drawn into the swapchain. This avoids both an unsafe write and permanently pausing FG for a persistent counter. Unknown/unreplayable options are passed through intact; the overlay is withheld rather than pausing a request that cannot later be restored.

The menu reports API results rather than claiming frame generation is active just because an option was selected. Actual frame synthesis still depends on the game's integration and the NVIDIA runtime accepting its requests.

## Validation and game test

The regression runner `tests/run_native_vulkan_dlssg.ps1` exercises production hook bodies with controlled Streamline entry points. It is separate from an in-game test and does not load NVIDIA's model runtime.

For Endfield, preserve the working NR DLL/model configuration, replace the loaded OptiScaler proxy DLL, enable native DLSS frame generation in the game, and enable NR manually in OptiScaler. Close the OptiScaler menu before comparing frame rates. Check both menu open/close cycles and transitions into/out of gameplay; retain `OptiScaler.log` for failures.

Previously supplied screenshots validate NR with Vulkan + DLSS SR on an RTX 4080 SUPER. They do not validate this later native-FG compatibility change. Results for this change must be recorded separately.
