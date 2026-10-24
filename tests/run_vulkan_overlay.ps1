# Extract and run the production MenuOverlayVk::QueuePresent body with fake Vulkan/UI services.
# No Vulkan ICD, window, GPU, Streamline, or game process is loaded.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$out = Join-Path ([IO.Path]::GetTempPath()) ('native-vulkan-overlay-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $out | Out-Null

function Extract($path, $signature) {
    $source = Get-Content -LiteralPath (Join-Path $repo $path) -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing production function: $signature" }
    $argsStart = $source.IndexOf('(', $start)
    $argsDepth = 0
    $bodyStart = $start
    for ($p = $argsStart; $p -lt $source.Length; $p++) {
        if ($source[$p] -eq '(') { $argsDepth++ }
        if ($source[$p] -eq ')') { $argsDepth-- }
        if (!$argsDepth) { $bodyStart = $p + 1; break }
    }
    $first = $source.IndexOf('{', $bodyStart)
    $depth = 0
    for ($p = $first; $p -lt $source.Length; $p++) {
        if ($source[$p] -eq '{') { $depth++ }
        if ($source[$p] -eq '}') { $depth-- }
        if (!$depth) { return $source.Substring($start, $p - $start + 1) }
    }
    throw "Unterminated production function: $signature"
}

Set-Content -LiteralPath "$out/native_vulkan_overlay_production.inc" `
    -Value (Extract 'OptiScaler/menu/menu_overlay_vk.cpp' 'bool MenuOverlayVk::QueuePresent(')
Set-Content -LiteralPath "$out/imgui_present_queue_production.inc" `
    -Value (Extract 'OptiScaler/include/imgui/imgui_impl_vulkan.cpp' 'bool ImGui_ImplVulkan_SetPresentQueue(')
$wsiSource = 'OptiScaler/hooks/Vulkan_Hooks.cpp'
$wsiCode = @('template <typename T>' + (Extract $wsiSource 'uint64_t HandleKey('))
$sourceText = Get-Content -LiteralPath "$repo/$wsiSource" -Raw
foreach ($name in @('SwapchainState', 'DeviceState')) {
    $match = [regex]::Match($sourceText, '(?s)struct ' + $name + '\s*\{.*?\n\};')
    if (!$match.Success) { throw "Missing WSI state $name" }
    $wsiCode += $match.Value
}
$wsiCode += @'
std::mutex wsiMutex, wsiHookMutex;
std::unordered_map<uint64_t, DeviceState> deviceStates;
VulkanWsi::Dispatch installedWsi;
'@
foreach ($signature in @(
    'bool VulkanHooks::GetDeviceContext(', 'bool VulkanHooks::GetGraphicsQueue(', 'bool VulkanHooks::GetPresentQueue(',
    'PFN_vkGetSwapchainImagesKHR VulkanHooks::GetSwapchainImages(',
    'bool VulkanHooks::IsVerifiedSwapchain(', 'void VulkanHooks::RecordSwapchain(',
    'bool VulkanHooks::ConfirmSwapchainImages(', 'void VulkanHooks::ForgetSwapchain(',
    'void VulkanHooks::ForgetDevice(', 'static void HookDevice(')) {
    $wsiCode += Extract $wsiSource $signature
}
Set-Content -LiteralPath "$out/vulkan_wsi_production.inc" -Value ($wsiCode -join "`n")

$savedPath = $env:PATH
try {
    $compilerCommand = Get-Command $Compiler -ErrorAction Stop
    $compilerPath = $compilerCommand.Source
    $env:PATH = (Split-Path $compilerPath) + [IO.Path]::PathSeparator + $env:PATH
    $exe = Join-Path $out 'native_vulkan_overlay.exe'
    & $compilerPath /nologo /std:c++20 /EHsc /W4 /DNOMINMAX `
        "/I$out" "/I$repo" "/I$repo/external/vulkan/include" "/Fe:$exe" "/Fo$out/" `
        "$PSScriptRoot/native_vulkan_overlay_smoke.cpp"
    if ($LASTEXITCODE) { throw 'Native Vulkan overlay test compilation failed.' }
    & $exe
    if ($LASTEXITCODE) { throw 'Native Vulkan overlay test failed.' }

    $backendExe = Join-Path $out 'native_imgui_present_queue.exe'
    & $compilerPath /nologo /std:c++20 /EHsc /W4 /DNOMINMAX `
        "/I$out" "/I$repo/external/vulkan/include" "/Fe:$backendExe" "/Fo$out/" `
        "$PSScriptRoot/native_imgui_present_queue_smoke.cpp"
    if ($LASTEXITCODE) { throw 'ImGui present queue test compilation failed.' }
    & $backendExe
    if ($LASTEXITCODE) { throw 'ImGui present queue test failed.' }

    $wsiExe = Join-Path $out 'native_vulkan_wsi.exe'
    & $compilerPath /nologo /std:c++20 /EHsc /W4 `
        "/I$out" "/I$repo/OptiScaler" "/I$repo/external/vulkan/include" "/Fe:$wsiExe" "/Fo$out/" `
        "$PSScriptRoot/native_vulkan_wsi_smoke.cpp"
    if ($LASTEXITCODE) { throw 'Vulkan WSI test compilation failed.' }
    & $wsiExe
    if ($LASTEXITCODE) { throw 'Vulkan WSI test failed.' }
} finally {
    $env:PATH = $savedPath
}
