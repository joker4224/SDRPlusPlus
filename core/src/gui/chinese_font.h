#pragma once
#include <imgui.h>

namespace style {
// BMP Chinese, including Extension A and compatibility ideographs. The old
// SimplifiedCommon range contains only 2500 selected ideographs.
inline const ImWchar* chineseGlyphRanges() {
    static const ImWchar ranges[] = {
        0x2E80, 0x303F, 0x3400, 0x4DBF, 0x4E00, 0x9FFF,
        0xF900, 0xFAFF, 0xFF00, 0xFFEF, 0
    };
    return ranges;
}

// Older ImGui has no raster-density setting. Rasterize the large mobile CJK
// set at a bounded size, then enlarge its geometry around the base baseline.
// This keeps UI sizing while limiting atlas/GPU memory; UVs stay unchanged.
inline void scaleChineseGlyphs(ImFont* font, float scale) {
    if (scale == 1.0f) { return; }
    for (auto& glyph : font->Glyphs) {
        bool chinese = false;
        for (const ImWchar* range = chineseGlyphRanges(); range[0]; range += 2) {
            if (glyph.Codepoint >= range[0] && glyph.Codepoint <= range[1]) { chinese = true; break; }
        }
        if (!chinese) { continue; }
        glyph.X0 *= scale;
        glyph.X1 *= scale;
        glyph.Y0 = font->Ascent + (glyph.Y0 - font->Ascent) * scale;
        glyph.Y1 = font->Ascent + (glyph.Y1 - font->Ascent) * scale;
        glyph.AdvanceX *= scale;
    }
    font->BuildLookupTable();
}
}
