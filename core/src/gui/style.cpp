#include <gui/style.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <config.h>
#include <utils/flog.h>
#include <cstdlib>
#include <filesystem>

namespace style {
    ImFont* baseFont;
    ImFont* bigFont;
    ImFont* hugeFont;
    ImVector<ImWchar> baseRanges;
    ImVector<ImWchar> bigRanges;
    ImVector<ImWchar> hugeRanges;

#ifndef __ANDROID__
    float uiScale = 1.0f;
#else
    float uiScale = 3.0f;
#endif

    bool loadFonts(std::string resDir) {
        ImFontAtlas* fonts = ImGui::GetIO().Fonts;
        if (!std::filesystem::is_directory(resDir)) {
            flog::error("Invalid resource directory: {0}", resDir);
            return false;
        }

        // Create base font range
        ImFontGlyphRangesBuilder baseBuilder;
        baseBuilder.AddRanges(fonts->GetGlyphRangesDefault());
        baseBuilder.AddRanges(fonts->GetGlyphRangesCyrillic());
        baseBuilder.BuildRanges(&baseRanges);

        // Create big font range
        ImFontGlyphRangesBuilder bigBuilder;
        const ImWchar bigRange[] = { '.', '9', 0 };
        bigBuilder.AddRanges(bigRange);
        bigBuilder.BuildRanges(&bigRanges);

        // Create huge font range
        ImFontGlyphRangesBuilder hugeBuilder;
        const ImWchar hugeRange[] = { 'S', 'S', 'D', 'D', 'R', 'R', '+', '+', ' ', ' ', 0 };
        hugeBuilder.AddRanges(hugeRange);
        hugeBuilder.BuildRanges(&hugeRanges);
        
        // Keep Roboto for the UI and merge platform Chinese glyphs into it.
        baseFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 16.0f * uiScale, NULL, baseRanges.Data);

#ifdef _WIN32
        const char* windowsDir = std::getenv("WINDIR");
        std::filesystem::path chineseFontPath = std::filesystem::path(windowsDir ? windowsDir : "C:/Windows") / "Fonts/msyh.ttc";
        if (std::filesystem::is_regular_file(chineseFontPath)) {
            ImFontConfig chineseFontConfig;
            chineseFontConfig.MergeMode = true;
            chineseFontConfig.PixelSnapH = true;
            if (!fonts->AddFontFromFileTTF(chineseFontPath.string().c_str(), 16.0f * uiScale, &chineseFontConfig, fonts->GetGlyphRangesChineseSimplifiedCommon())) {
                flog::warn("Failed to load Chinese font: {0}", chineseFontPath.string());
            }
        }
        else {
            flog::warn("Chinese font not found: {0}", chineseFontPath.string());
        }
#elif defined(__ANDROID__)
        // Android ships CJK fallback fonts outside the app's Roboto assets.
        // Merge only the Chinese glyphs into the base font used by bookmarks.
        struct ChineseFontCandidate { const char* path; int fontIndex; };
        const ChineseFontCandidate chineseFonts[] = {
            { "/system/fonts/NotoSansCJK-Regular.ttc", 2 }, // Simplified Chinese face
            { "/system/fonts/NotoSansSC-Regular.otf", 0 },
            { "/system/fonts/DroidSansFallback.ttf", 0 }
        };
        bool chineseFontLoaded = false;
        for (const auto& candidate : chineseFonts) {
            if (!std::filesystem::is_regular_file(candidate.path)) { continue; }
            ImFontConfig chineseFontConfig;
            chineseFontConfig.MergeMode = true;
            chineseFontConfig.PixelSnapH = true;
            chineseFontConfig.FontNo = candidate.fontIndex;
            if (fonts->AddFontFromFileTTF(candidate.path, 16.0f * uiScale, &chineseFontConfig, fonts->GetGlyphRangesChineseSimplifiedCommon())) {
                chineseFontLoaded = true;
                break;
            }
        }
        if (!chineseFontLoaded) {
            flog::warn("No Android Chinese fallback font found");
        }
#endif

        // Add bigger fonts for frequency select and title
        bigFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 45.0f * uiScale, NULL, bigRanges.Data);
        hugeFont = fonts->AddFontFromFileTTF(((std::string)(resDir + "/fonts/Roboto-Medium.ttf")).c_str(), 128.0f * uiScale, NULL, hugeRanges.Data);

        return true;
    }

    void beginDisabled() {
        ImGui::PushItemFlag(ImGuiItemFlags_Disabled, true);
        auto& style = ImGui::GetStyle();
        ImVec4* colors = style.Colors;
        ImVec4 btnCol = colors[ImGuiCol_Button];
        ImVec4 frameCol = colors[ImGuiCol_FrameBg];
        ImVec4 textCol = colors[ImGuiCol_Text];
        btnCol.w = 0.15f;
        frameCol.w = 0.30f;
        textCol.w = 0.65f;
        ImGui::PushStyleColor(ImGuiCol_Button, btnCol);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, frameCol);
        ImGui::PushStyleColor(ImGuiCol_Text, textCol);
    }

    void endDisabled() {
        ImGui::PopItemFlag();
        ImGui::PopStyleColor(3);
    }
}

namespace ImGui {
    void LeftLabel(const char* text) {
        float vpos = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(vpos + GImGui->Style.FramePadding.y);
        ImGui::TextUnformatted(text);
        ImGui::SameLine();
        ImGui::SetCursorPosY(vpos);
    }

    void FillWidth() {
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    }
}
