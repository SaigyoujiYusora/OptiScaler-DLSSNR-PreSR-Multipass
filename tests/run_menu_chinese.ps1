$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot -Parent
$out = Join-Path ([IO.Path]::GetTempPath()) ('optiscaler-chinese-' + [guid]::NewGuid())
New-Item -ItemType Directory -Path $out | Out-Null
$source = Get-Content "$repo/OptiScaler/menu/menu_common.cpp" -Raw
$start = $source.IndexOf('const char* UiText(')
$end = $source.IndexOf('} // namespace', $start)
if ($start -lt 0 -or $end -le $start) { throw 'Cannot locate production UI localization.' }
Set-Content "$out/ChineseUi.cpp" -Value $source.Substring($start, $end - $start) -Encoding utf8
$imgui = "$repo/OptiScaler/include/imgui"
& cl.exe /nologo /utf-8 /std:c++20 /EHsc "/I$out" "/I$imgui" "/I$repo/OptiScaler/menu" "/I$repo/external/freetype" `
    "/Fo$out/" "/Fe$out/menu-chinese.exe" "$PSScriptRoot/menu_chinese_smoke.cpp" `
    "$imgui/imgui.cpp" "$imgui/imgui_draw.cpp" "$imgui/imgui_tables.cpp" "$imgui/imgui_widgets.cpp" `
    "$imgui/misc/freetype/imgui_freetype.cpp" /link "$repo/external/freetype/freetype.lib" user32.lib gdi32.lib imm32.lib dwmapi.lib
if ($LASTEXITCODE) { throw 'Chinese menu regression did not compile.' }
& "$out/menu-chinese.exe"
if ($LASTEXITCODE) { throw 'Chinese menu regression failed.' }
