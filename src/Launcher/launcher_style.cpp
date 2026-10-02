#include "launcher_style.h"
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace band3::launcher {

namespace {

// the sizes the launcher's font is baked at: the SDK's ImGui renderer can't
// bake new sizes on the fly, so each size is drawn from the nearest bake
// at or above it
constexpr float kFontBakes[] = {17, 20, 23, 30, 40, 60};
std::array<ImFont*, std::size(kFontBakes)> g_fonts{};

// the page's scale, set at the start of each frame
float g_scale = 1.0f;

ImFont* FontFor(float size) {
    ImFont* best = nullptr;
    for (size_t i = 0; i < g_fonts.size(); i++) {
        if (!g_fonts[i]) continue;
        best = g_fonts[i];
        if (kFontBakes[i] >= size - 0.5f) break;
    }
    return best;
}

// a readable system font, if there's one where it's expected
std::filesystem::path FindUiFont() {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::filesystem::path fonts = std::filesystem::path(windows) / "Fonts";
        // Segoe UI on Windows; Proton (a Steam Deck) has Arial's metric twin
        for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf"}) {
            candidates.push_back(fonts / name);
        }
    }
#else
    for (const char* file : {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        candidates.emplace_back(file);
    }
#endif
    for (const auto& file : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) return file;
    }
    return {};
}

std::vector<char> ReadFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

}

float ScaleFor(const ImVec2& display) {
    const float s = std::min(display.x / 1280.0f, display.y / 720.0f);
    return std::clamp(s, 0.75f, 3.0f);
}

void SetScale(float scale) { g_scale = scale; }

float Scale() { return g_scale; }

void ApplyStyle(ImGuiStyle& style, float s) {
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;
    style.WindowPadding = ImVec2(28 * s, 18 * s);
    style.FramePadding = ImVec2(10 * s, 6 * s);
    style.ItemSpacing = ImVec2(12 * s, 9 * s);
    style.ItemInnerSpacing = ImVec2(8 * s, 6 * s);
    style.CellPadding = ImVec2(6 * s, 5 * s);
    style.IndentSpacing = 20 * s;
    style.ScrollbarSize = 14 * s;
    style.GrabMinSize = 16 * s;
    style.WindowRounding = 0;
    style.ChildRounding = 6 * s;
    style.FrameRounding = 5 * s;
    style.PopupRounding = 6 * s;
    style.ScrollbarRounding = 7 * s;
    style.GrabRounding = 4 * s;
    style.TabRounding = 5 * s;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 2 * s;
    style.TabBarOverlineSize = 2 * s;
    style.SeparatorTextBorderSize = 1;
    style.DisabledAlpha = 0.45f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = kBackground;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPopup;
    c[ImGuiCol_Border] = kLine;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = kAccentDim;
    c[ImGuiCol_HeaderHovered] = kFrameHover;
    c[ImGuiCol_HeaderActive] = kFrameActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = kFrame;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = kFrame;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
    c[ImGuiCol_ScrollbarGrabActive] = kFrameActive;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_TitleBg] = kFrame;
    c[ImGuiCol_TitleBgActive] = kAccentDim;
    c[ImGuiCol_TitleBgCollapsed] = kFrame;
}

FontScope::FontScope(float size) {
    const float px = Px(size);
    ImGui::PushFont(FontFor(px), px);
}

void AddLauncherFonts(ImFontAtlas* atlas) {
    // Latin, Latin-1 and Latin Extended-A, and punctuation (dashes, quotes, ...)
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x205E, 0};
    const std::filesystem::path file = FindUiFont();
    std::vector<char> data = file.empty() ? std::vector<char>() : ReadFile(file);

    // one copy of the font's data, owned by the first font, for every size
    void* shared = nullptr;
    int shared_size = 0;
    bool owned = false;
    if (!data.empty()) {
        shared_size = static_cast<int>(data.size());
        shared = IM_ALLOC(data.size());
        std::memcpy(shared, data.data(), data.size());
        owned = true;
    } else if (atlas->Sources.Size > 0 && atlas->Sources[0].FontData) {
        // the SDK's own font, larger
        shared = atlas->Sources[0].FontData;
        shared_size = atlas->Sources[0].FontDataSize;
    }
    if (!shared) {
        REXLOG_WARN("Launcher: no font to draw with; it uses the SDK's small one");
        return;
    }
    for (size_t i = 0; i < std::size(kFontBakes); i++) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = owned && i == 0;
        std::snprintf(config.Name, sizeof(config.Name), "band3 launcher %.0fpx", kFontBakes[i]);
        g_fonts[i] = atlas->AddFontFromMemoryTTF(shared, shared_size, kFontBakes[i], &config,
                                                 kRanges);
        if (!g_fonts[i] && i == 0 && owned) IM_FREE(shared);
        if (!g_fonts[i]) break;
    }
    REXLOG_INFO("Launcher: font {}",
                data.empty() ? std::string("the SDK's own") : rex::path_to_utf8(file));
}

}
