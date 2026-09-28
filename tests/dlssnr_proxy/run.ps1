# Run from a Visual Studio developer PowerShell with cl.exe on PATH.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot '../..')).Path
$build = Join-Path ([System.IO.Path]::GetTempPath()) ('optiscaler-dlssnr-tests-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path (Join-Path $build 'proxies') -Force | Out-Null
foreach ($header in @('pch.h', 'Config.h', 'Logger.h', 'd3d12.h', 'proxies/NVNGX_Proxy.h')) {
    Set-Content -LiteralPath (Join-Path $build $header) -Value '// Dependency supplied by MockNgx.h.'
}
if ([IO.Path]::GetFileName($Compiler) -match 'g\+\+|clang\+\+') {
    & $Compiler -std=c++20 -Wall -Wextra -include "$PSScriptRoot/MockNgx.h" "-I$build" `
        "-I$repo/external/nvngx_dlss_sdk" "-I$repo/external/vulkan/include" `
        "$PSScriptRoot/ProxyTests.cpp" -o "$build/ProxyTests.exe"
} else {
    & $Compiler /nologo /std:c++20 /EHsc /W4 "/FI$PSScriptRoot/MockNgx.h" "/I$build" `
        "/I$repo/external/nvngx_dlss_sdk" "/I$repo/external/vulkan/include" `
        "/Fo$build/ProxyTests.obj" "/Fe$build/ProxyTests.exe" "$PSScriptRoot/ProxyTests.cpp"
}
if ($LASTEXITCODE -ne 0) { throw 'DLSS-NR proxy test compilation failed.' }
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path (Get-Command $Compiler).Source) + [IO.Path]::PathSeparator + $env:PATH
    & "$build/ProxyTests.exe"
    if ($LASTEXITCODE -ne 0) { throw 'DLSS-NR proxy regression tests failed.' }
} finally { $env:PATH = $savedPath }
Write-Output 'DLSS-NR proxy regression tests passed.'
