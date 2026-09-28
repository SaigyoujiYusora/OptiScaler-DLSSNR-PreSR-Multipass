# Builds and runs the production Vulkan CompatibilityRuntime against local fake DLLs.
# No NVIDIA DLL, Vulkan loader, GPU, or game process is used.
param(
    [string]$Compiler = 'g++.exe'
)
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$compilerCommand = Get-Command $Compiler -ErrorAction Stop
$compilerPath = $compilerCommand.Source
$compilerBin = Split-Path -Parent $compilerPath
$previousPath = $env:PATH
$build = Join-Path ([IO.Path]::GetTempPath()) ('nr-vulkan-compatibility-' + [guid]::NewGuid())
$fake = Join-Path $build 'fake'
New-Item -ItemType Directory -Path $build,$fake | Out-Null

# The production translation unit includes the normal Windows/DX12 headers, but
# its logger is irrelevant to this test. Keep the build independent of MSVC's
# precompiled-header and formatting implementation.
Set-Content -LiteralPath (Join-Path $build 'pch.h') -Value '#pragma once'
Set-Content -LiteralPath (Join-Path $build 'Logger.h') -Value @'
#pragma once
#define LOG_INFO(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
'@

$sdk = Join-Path $repo 'external/nvngx_dlss_sdk'
$vulkan = Join-Path $repo 'external/vulkan/include'
$include = @("-I$build", "-I$sdk", "-I$vulkan", "-I$(Join-Path $repo 'OptiScaler')")
$common = @('-std=c++20', '-O2', '-Wall', '-Wextra', '-Wno-unknown-pragmas') + $include

Push-Location $build
try {
    & $compilerPath @common '-shared' '-static-libgcc' '-static-libstdc++' `
        (Join-Path $PSScriptRoot 'FakeNvngxVulkan.cpp') '-o' (Join-Path $fake 'nvngx_dlssnr.dll')
    if ($LASTEXITCODE) { throw 'Complete fake Vulkan DLL compilation failed.' }

    & $compilerPath @common '-shared' '-static-libgcc' '-static-libstdc++' '-DFAKE_MISSING_EVALUATE' `
        (Join-Path $PSScriptRoot 'FakeNvngxVulkan.cpp') '-o' (Join-Path $fake 'missing_eval_nvngx_dlssnr.dll')
    if ($LASTEXITCODE) { throw 'Missing-evaluate fake Vulkan DLL compilation failed.' }

    & $compilerPath @common '-shared' '-static-libgcc' '-static-libstdc++' '-DFAKE_MISSING_SHUTDOWN' `
        (Join-Path $PSScriptRoot 'FakeNvngxVulkan.cpp') '-o' (Join-Path $fake 'missing_shutdown_nvngx_dlssnr.dll')
    if ($LASTEXITCODE) { throw 'Missing-shutdown fake Vulkan DLL compilation failed.' }

    Copy-Item -LiteralPath (Join-Path $fake 'nvngx_dlssnr.dll') -Destination (Join-Path $fake 'nvngx_dlssnr_alt.dll')

    $runtime = Join-Path $repo 'OptiScaler/dlssnr/DlssNr_CompatibilityRuntime.cpp'
    $smoke = Join-Path $PSScriptRoot 'VulkanCompatibilitySmoke.cpp'
    $exe = Join-Path $build 'VulkanCompatibilitySmoke.exe'
    & $compilerPath @common '-static-libgcc' '-static-libstdc++' '-municode' $smoke $runtime '-lpsapi' '-o' $exe
    if ($LASTEXITCODE) { throw 'Production Vulkan CompatibilityRuntime test compilation failed.' }

    $env:PATH = "$compilerBin;$env:PATH"
    & $exe (Join-Path $fake 'nvngx_dlssnr.dll') (Join-Path $fake 'missing_eval_nvngx_dlssnr.dll') `
        (Join-Path $fake 'missing_shutdown_nvngx_dlssnr.dll') (Join-Path $fake 'nvngx_dlssnr_alt.dll')
    if ($LASTEXITCODE) { throw 'Production Vulkan CompatibilityRuntime test failed.' }
}
finally {
    $env:PATH = $previousPath
    Pop-Location
}
