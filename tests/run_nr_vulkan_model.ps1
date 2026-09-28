# Runs production model functions with counted dependencies; no GPU/runtime is loaded.
param([string]$Compiler = 'cl.exe')
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path "$PSScriptRoot/..").Path
$out = Join-Path ([IO.Path]::GetTempPath()) ('nr-vulkan-model-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $out | Out-Null
function Extract($path, $signature) {
    $source = Get-Content -LiteralPath (Join-Path $repo $path) -Raw
    $start = $source.IndexOf($signature, [StringComparison]::Ordinal)
    if ($start -lt 0) { throw "Missing production function: $signature" }
    $bodyStart = $start
    if (!$signature.StartsWith('class ')) {
        # Default arguments can contain {} before the function body.
        $argsStart = $source.IndexOf('(', $start)
        $argsDepth = 0
        for ($p = $argsStart; $p -lt $source.Length; $p++) {
            if ($source[$p] -eq '(') { $argsDepth++ }
            if ($source[$p] -eq ')') { $argsDepth-- }
            if (!$argsDepth) { $bodyStart = $p + 1; break }
        }
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
$source = 'OptiScaler/dlssnr/DlssNrFeature_Vk_Model.cpp'
$functions = @('void ModelVk::Impl::Fail(', 'void ModelVk::Impl::ReleaseModels(',
    'bool ModelVk::Impl::CreateModel(', 'NVSDK_NGX_Result ModelVk::Impl::EvaluateModel(')
Set-Content -LiteralPath "$out/vulkan_model_production.inc" -Value (($functions | ForEach-Object { Extract $source $_ }) -join "`n")
$pipelineSource = 'OptiScaler/dlssnr/DlssNrPipeline_Vk.h'
$pipelineFunctions = @('inline VkImageInfo ImageInfo(', 'inline NVSDK_NGX_Resource_VK WrapImage(',
    'inline VkImageInfo ParameterImage(', 'inline ShaderPass_Vk MakePass(', 'inline VkImageInfo PrepareInput(')
$code = @($pipelineFunctions | ForEach-Object { Extract $pipelineSource $_ })
$code += (Extract $pipelineSource 'class ScopedVkParameters') + ';'
Set-Content -LiteralPath "$out/vulkan_presr_production.inc" -Value ($code -join "`n")
$shaderPipeline = Get-Content -LiteralPath "$repo/OptiScaler/upscalers/ShaderPipeline_Vk.h" -Raw
Set-Content -LiteralPath "$out/vulkan_shader_pipeline.inc" -Value ($shaderPipeline -replace '(?m)^#(?:include|pragma)[^\r\n]*', '')
$evaluate = Extract 'OptiScaler/upscalers/IFeature_Vk.cpp' 'bool IFeature_Vk::Evaluate('
$start = $evaluate.IndexOf('    if (nrBeforeUpscale)', [StringComparison]::Ordinal)
if ($start -lt 0) { throw 'Missing production pre-SR call site.' }
Set-Content -LiteralPath "$out/vulkan_evaluate_tail.inc" -Value $evaluate.Substring($start, $evaluate.LastIndexOf('}') - $start)
$savedPath = $env:PATH
try {
    $env:PATH = (Split-Path (Get-Command $Compiler).Source) + [IO.Path]::PathSeparator + $env:PATH
    foreach ($test in @('nr_vulkan_model_smoke', 'nr_vulkan_presr_smoke')) {
        $exe = "$out/$test.exe"
        if ([IO.Path]::GetFileName($Compiler) -match 'g\+\+|clang\+\+') {
            & $Compiler -std=c++20 -Wall -Wextra -pedantic "-I$out" "$PSScriptRoot/$test.cpp" -o $exe
        } else {
            & $Compiler /nologo /std:c++20 /EHsc /W4 "/I$out" "/Fo$out/" "/Fe$exe" "$PSScriptRoot/$test.cpp"
        }
        if ($LASTEXITCODE) { throw "$test compilation failed." }
        & $exe
        if ($LASTEXITCODE) { throw "$test failed." }
    }
} finally { $env:PATH = $savedPath }
