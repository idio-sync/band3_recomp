#include "theme.h"
#include <rex/logging.h>
#include <algorithm>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <utility>
#include "kMontserratExtraBold.h"
#include "kMontserratSemiBold.h"
#include "src/Launcher/device_lists.h"
#include "src/Launcher/launcher_style.h"

namespace band3::launcher::ui {

namespace {

// the bakes, as PlanFontBakes planned them, with their ImGui fonts
std::vector<FontBake> g_bakes;
std::vector<ImFont*> g_fonts;

// ImGui keeps the ranges it's given until the atlas is built
std::vector<ImWchar> g_full_ranges;
std::vector<ImWchar> g_title_ranges;

float BiggestMonitorScale() {
    std::vector<std::pair<int, int>> sizes;
    for (const Monitor& m : ListMonitors()) {
        // the largest mode it has: exclusive fullscreen can use it
        int w = m.current.width, h = m.current.height;
        for (const DisplayMode& mode : m.modes) {
            if (mode.width * mode.height > w * h) {
                w = mode.width;
                h = mode.height;
            }
        }
        sizes.emplace_back(w, h);
    }
    return MaxScaleFor(sizes);
}

ImFont* FontFor(const TextStyle& style, float px, bool title) {
    const FontBake* bake = PickBake(g_bakes, style.weight, px, title);
    if (!bake) return nullptr;
    return g_fonts[size_t(bake - g_bakes.data())];
}

// the line's pixel height and the font's
float LinePx(const TextStyle& style) { return Px(style.size); }
float FontPx(const TextStyle& style) { return Px(style.size) * kFontToLine; }

}

void AddThemeFonts(ImFontAtlas* atlas) {
    g_full_ranges.assign(std::begin(kFullRanges), std::end(kFullRanges));
    g_full_ranges.push_back(0);
    g_title_ranges.clear();
    for (const char* c = kTitleGlyphs; *c; c++) {
        g_title_ranges.push_back(ImWchar(*c));
        g_title_ranges.push_back(ImWchar(*c));
    }
    g_title_ranges.push_back(0);

    const float scale = BiggestMonitorScale();
    g_bakes = PlanFontBakes(scale);
    g_fonts.assign(g_bakes.size(), nullptr);
    for (size_t i = 0; i < g_bakes.size(); i++) {
        const FontBake& bake = g_bakes[i];
        const bool bold = bake.weight == FontWeight::kExtraBold;
        ImFontConfig config;
        // the data is band3's own, compiled in
        config.FontDataOwnedByAtlas = false;
        std::snprintf(config.Name, sizeof(config.Name), "Montserrat %s %.0fpx%s",
                      bold ? "ExtraBold" : "SemiBold", bake.px, bake.titles_only ? " titles" : "");
        void* data = const_cast<unsigned char*>(bold ? kMontserratExtraBold : kMontserratSemiBold);
        const int size = int(bold ? kMontserratExtraBold_size : kMontserratSemiBold_size);
        g_fonts[i] = atlas->AddFontFromMemoryTTF(
            data, size, bake.px, &config,
            bake.titles_only ? g_title_ranges.data() : g_full_ranges.data());
    }
    REXLOG_INFO("Launcher: Montserrat in {} sizes, for a window up to {:.2f} times 1280x720",
                g_bakes.size(), scale);
}

std::string Shown(const TextStyle& style, std::string_view text) {
    std::string out(text);
    if (style.caps) {
        // ASCII letters; the rest (UTF-8) stays as written
        for (char& c : out) {
            if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        }
    }
    return out;
}

float TextWidth(const TextStyle& style, std::string_view text) {
    const std::string shown = Shown(style, text);
    ImFont* font = FontFor(style, FontPx(style), IsTitleText(shown));
    if (!font) return 0;
    return font->CalcTextSizeA(FontPx(style), FLT_MAX, 0, shown.data(), shown.data() + shown.size()).x;
}

float PaintText(ImDrawList* dl, ImVec2 pos, const TextStyle& style, ImU32 color,
               std::string_view text, float max_width) {
    std::string shown = Shown(style, text);
    const float px = FontPx(style);
    ImFont* font = FontFor(style, px, IsTitleText(shown));
    if (!font || shown.empty()) return 0;
    const auto width = [&](const std::string& s) {
        return font->CalcTextSizeA(px, FLT_MAX, 0, s.data(), s.data() + s.size()).x;
    };
    float w = width(shown);
    if (max_width > 0 && w > max_width) {
        // cut at a character boundary (not inside a UTF-8 sequence) with an ellipsis
        const std::string ellipsis = "\xE2\x80\xA6";
        while (!shown.empty()) {
            shown.pop_back();
            while (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0x80) {
                shown.pop_back();
            }
            if (!shown.empty() && (static_cast<unsigned char>(shown.back()) & 0xC0) == 0xC0) {
                shown.pop_back();
            }
            while (!shown.empty() && shown.back() == ' ') shown.pop_back();
            if (width(shown + ellipsis) <= max_width) break;
        }
        shown += ellipsis;
        // the ellipsis isn't in a titles-only bake
        font = FontFor(style, px, false);
        w = width(shown);
    }
    // the glyphs centred in the line
    const float y = pos.y + (LinePx(style) - px) * 0.5f;
    dl->AddText(font, px, ImVec2(std::round(pos.x), std::round(y)), color, shown.data(),
                shown.data() + shown.size());
    return w;
}

void PushThemeFont(const TextStyle& style) {
    const float px = FontPx(style);
    ImGui::PushFont(FontFor(style, px, false), px);
}

std::vector<std::string> WrapText(const TextStyle& style, std::string_view text, float width) {
    std::vector<std::string> lines;
    std::string line;
    size_t at = 0;
    while (at <= text.size()) {
        size_t end = text.find(' ', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string word(text.substr(at, end - at));
        const std::string candidate = line.empty() ? word : line + " " + word;
        if (!line.empty() && TextWidth(style, candidate) > width) {
            lines.push_back(line);
            line = word;
        } else {
            line = candidate;
        }
        at = end + 1;
        // a word wider than a line (a path) breaks inside it, after a
        // separator where there's one, so nothing runs past the edge
        while (TextWidth(style, line) > width && line.size() > 1) {
            size_t fit = 0;
            for (size_t i = 1; i <= line.size(); i++) {
                if (i < line.size() && (static_cast<unsigned char>(line[i]) & 0xC0) == 0x80) continue;
                if (TextWidth(style, std::string_view(line).substr(0, i)) > width) break;
                fit = i;
            }
            if (fit == 0) break;
            const size_t sep = line.find_last_of("\\/_-,.", fit - 1);
            const size_t cut = sep != std::string::npos && sep + 1 > fit / 3 ? sep + 1 : fit;
            lines.push_back(line.substr(0, cut));
            line.erase(0, cut);
        }
    }
    if (!line.empty()) lines.push_back(line);
    return lines;
}

}
