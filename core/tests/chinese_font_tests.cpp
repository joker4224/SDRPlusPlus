#include <gui/chinese_font.h>
#include <imgui.h>
#include <cmath>
#include <cstdio>

// Standalone ImGui atlas test; pass Roboto and a CJK font path as arguments.
int main(int argc, char** argv) {
    if (argc != 3) { return 2; }
    ImGui::CreateContext();
    auto* atlas = ImGui::GetIO().Fonts;
    atlas->Flags |= ImFontAtlasFlags_NoPowerOfTwoHeight;
    atlas->TexDesiredWidth = 4096;
    auto* font = atlas->AddFontFromFileTTF(argv[1], 48.0f);
    ImFontConfig config;
    config.MergeMode = true;
    config.OversampleH = config.OversampleV = 1;
    if (!font || !atlas->AddFontFromFileTTF(argv[2], 20.0f, &config, style::chineseGlyphRanges()) || !atlas->Build()) { return 1; }
    // Traditional and uncommon characters omitted from SimplifiedCommon.
    const ImWchar text[] = {0x4E2D, 0x6587, 0x5F67, 0x7FCA, 0x9F98, 0x912E, 0x932B, 0x3400, 0};
    int checked = 0;
    for (auto cp : text) {
        if (!cp) break;
        const auto* glyph = font->FindGlyphNoFallback(cp);
        if (!glyph) { std::printf("Missing glyph U+%04X\n", cp); return 1; }
        ++checked;
    }
    const float advance = font->FindGlyphNoFallback(0x4E2D)->AdvanceX;
    const float ascii = font->FindGlyphNoFallback('A')->AdvanceX;
    style::scaleChineseGlyphs(font, 2.4f);
    if (std::fabs(font->GetCharAdvance(0x4E2D) - advance * 2.4f) > 0.01f ||
        font->GetCharAdvance('A') != ascii || atlas->TexWidth > 4096 || atlas->TexHeight > 4096) { return 1; }
    std::printf("PASS: %d common/uncommon/traditional/Extension-A glyphs, scaled advance cache, unchanged Latin; atlas %dx%d <= 4096\n", checked, atlas->TexWidth, atlas->TexHeight);
    ImGui::DestroyContext();
}
