# Compile extracted production Streamline hook bodies with fake services.
# No Streamline DLL, NGX DLL, GPU, or game process is loaded.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$out = Join-Path ([IO.Path]::GetTempPath()) ('native-vulkan-dlssg-' + [guid]::NewGuid())
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

$source = 'OptiScaler/hooks/Streamline_Hooks.cpp'
$functions = @(
    'size_t DlssgOptionsSize(',
    'sl::Result StreamlineHooks::hkslInit(',
    'sl::Result StreamlineHooks::hkslIsFeatureSupported(',
    'sl::Result StreamlineHooks::hkslIsFeatureLoaded(',
    'sl::Result StreamlineHooks::hkslGetFeatureRequirements(',
    'sl::Result StreamlineHooks::hkslGetFeatureVersion(',
    'sl::Result StreamlineHooks::hkslGetFeatureFunction(',
    'sl::Result StreamlineHooks::hkslEvaluateFeature(',
    'sl::Result StreamlineHooks::hkslPCLSetMarker(',
    'sl::Result StreamlineHooks::hkslSetConstants(',
    'bool StreamlineHooks::hkdlssg_slOnPluginLoad(',
    'sl::Result StreamlineHooks::hkslDLSSGSetOptions(',
    'sl::Result StreamlineHooks::hkslDLSSGGetState(',
    'bool StreamlineHooks::IsNativeVulkanDlssg(',
    'StreamlineHooks::NativeVulkanDlssgStatus StreamlineHooks::GetNativeVulkanDlssgStatus(',
    'bool StreamlineHooks::SyncNativeVulkanDlssgMenu(',
    'void StreamlineHooks::applyMenuDlssgInterlock('
)
Set-Content -LiteralPath "$out/native_vulkan_dlssg_production.inc" -Value (($functions | ForEach-Object { Extract $source $_ }) -join "`n")

$savedPath = $env:PATH
try {
    $compilerCommand = Get-Command $Compiler -ErrorAction Stop
    $compilerPath = $compilerCommand.Source
    $env:PATH = (Split-Path $compilerPath) + [IO.Path]::PathSeparator + $env:PATH
    $exe = Join-Path $out 'native_vulkan_dlssg.exe'
    & $compilerPath /nologo /std:c++20 /EHsc /W4 /DNOMINMAX `
        "/I$out" "/I$repo" "/I$repo/OptiScaler" "/I$repo/external/streamline" "/I$repo/external/streamline1" `
        "/I$repo/external/nlohmann" "/I$repo/external/vulkan/include" `
        "/I$repo/external/nvngx_dlss_sdk" "/I$repo/external/spdlog/include" "/Fe:$exe" "/Fo$out/" `
        "$PSScriptRoot/native_vulkan_dlssg_smoke.cpp"
    if ($LASTEXITCODE) { throw 'Native Vulkan Streamline DLSSG test compilation failed.' }
    & $exe
    if ($LASTEXITCODE) { throw 'Native Vulkan Streamline DLSSG test failed.' }
} finally {
    $env:PATH = $savedPath
}
