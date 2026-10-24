#define NOMINMAX
#include <windows.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <array>
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>
#include <font/Hack_Compressed.h>

std::string wstring_to_string(const std::wstring& text)
{
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string output(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), output.data(), size, nullptr, nullptr);
    return output;
}
#include "ChineseUi.cpp"

int main()
{
    const char* first = UiText("Frame Generation");
    const char* second = UiText("Default");
    assert(std::string_view(first) == "帧生成" && std::string_view(second) == "默认");
    assert(UiText(nullptr) == nullptr);
    for (const auto* label : { "Frame Generation", "Change Upscaler##2", "Frame timing fix##ada", "##UpscalerCombo" })
    {
        const std::string identity = label[0] == '#' ? label : std::string("###") + label;
        assert(ImHashStr(identity.c_str()) == ImHashStr(UiLabel(label)));
    }
    assert(std::string_view(UiLabel("Change Upscaler##2")).find("更换") != std::string_view::npos);
    for (bool hq : { false, true })
    {
        ImGui::CreateContext();
        auto& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.DisplaySize = { 1280, 720 };
        io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
        if (hq)
            io.FontDefault = io.Fonts->AddFontFromMemoryCompressedBase85TTF(hack_compressed_compressed_data_base85, 20);
        else
            io.FontDefault = io.Fonts->AddFontDefault();
        assert(AddChineseFont(io.Fonts, 20));
        ImGui::NewFrame();
        ImGui::SetNextWindowPos({ 0, 0 });
        ImGui::SetNextWindowSize({ 1000, 600 });
        ImGui::Begin("Chinese font regression");
        ImGui::TextUnformatted("中文帧生成：神经渲染、光晕抑制（实验）、显示中心轮廓");
        for (ImWchar glyph : { 0x4E2D, 0x6587, 0x5E27, 0x795E, 0x7ECF, 0x6E32, 0x67D3, 0xFF08, 0xFF09, 0x8F6E, 0x5ED3 })
            assert(ImGui::GetFontBaked()->FindGlyphNoFallback(glyph));
        ImGui::End();
        ImGui::Render();
        assert(ImGui::GetDrawData()->TotalVtxCount > 0);
        ImGui::DestroyContext();
    }
    std::puts("PASS: Chinese labels have stable IDs/storage; real Windows CJK glyphs render with HQ on and off");
}
