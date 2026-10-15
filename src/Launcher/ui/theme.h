#pragma once
#include <string>
#include <string_view>
#include <vector>
#include <imgui.h>
#include "font_plan.h"

// The redesigned launcher's look, after Rock Band 3's menus: its colours, its
// text sizes (at a 1280x720 window, scaled with launcher_style.h's Px), and
// its type, Montserrat (SIL Open Font License, src/ThirdParty/montserrat)
// in two weights: ExtraBold for caps, SemiBold for sentence case. The
// textures come from the game (skin.h); these are what's drawn without them.

namespace band3::launcher::ui {

// Colours

namespace color {
// the background's navy, darker at the edges
inline constexpr ImU32 kNavyDeep = IM_COL32(2, 4, 26, 255);
inline constexpr ImU32 kNavy = IM_COL32(5, 10, 44, 255);
inline constexpr ImU32 kNavyGlow = IM_COL32(32, 52, 150, 110);
inline constexpr ImU32 kText = IM_COL32(242, 242, 242, 255);
inline constexpr ImU32 kSoft = IM_COL32(201, 204, 214, 255);
inline constexpr ImU32 kMuted = IM_COL32(163, 170, 187, 255);
// the big titles, faded grey
inline constexpr ImU32 kTitle = IM_COL32(205, 208, 220, 107);
// text on the yellow selection bar
inline constexpr ImU32 kOnYellow = IM_COL32(23, 23, 10, 255);
inline constexpr ImU32 kYellow = IM_COL32(230, 220, 31, 255);
inline constexpr ImU32 kTeal = IM_COL32(70, 150, 165, 128);
inline constexpr ImU32 kTealEdge = IM_COL32(120, 200, 215, 90);
inline constexpr ImU32 kActionRed = IM_COL32(150, 40, 30, 158);
inline constexpr ImU32 kRowShade = IM_COL32(0, 0, 0, 107);
inline constexpr ImU32 kPanelFill = IM_COL32(4, 8, 22, 214);
inline constexpr ImU32 kFrameBlue = IM_COL32(90, 150, 220, 255);
inline constexpr ImU32 kChanged = IM_COL32(240, 138, 29, 255);
inline constexpr ImU32 kGood = IM_COL32(143, 224, 143, 255);
inline constexpr ImU32 kWarn = IM_COL32(255, 190, 92, 255);
inline constexpr ImU32 kBad = IM_COL32(255, 125, 112, 255);
inline constexpr ImU32 kInfo = IM_COL32(159, 208, 255, 255);
inline constexpr ImU32 kOn = IM_COL32(215, 232, 106, 255);
// the help bar's dashes, the guitar's frets
inline constexpr ImU32 kFretGreen = IM_COL32(63, 191, 63, 255);
inline constexpr ImU32 kFretRed = IM_COL32(216, 58, 58, 255);
inline constexpr ImU32 kFretYellow = IM_COL32(230, 220, 31, 255);
inline constexpr ImU32 kFretBlue = IM_COL32(47, 127, 224, 255);
inline constexpr ImU32 kFretOrange = IM_COL32(240, 138, 29, 255);
}

// Text sizes, as line heights at 1280x720

namespace text {
inline constexpr float kHubTitle = 96;
inline constexpr float kTitle = 80;
inline constexpr float kHubMenu = 50;
inline constexpr float kBoxTitle = 30;
inline constexpr float kPanelHeading = 28;
inline constexpr float kCategory = 26;
inline constexpr float kRow = 25;
inline constexpr float kValue = 23;
inline constexpr float kBand = 23;
inline constexpr float kStrip = 23;
inline constexpr float kHelp = 21;
inline constexpr float kBody = 20;
inline constexpr float kChip = 17;
inline constexpr float kKey = 16;
// a body line's height, wrapped
inline constexpr float kBodyLine = 24;
}

// Montserrat's glyphs are this much of a line's height, which matches the
// cap height the layout was drawn with
inline constexpr float kFontToLine = 0.8f;

// Adds Montserrat's bakes (PlanFontBakes) for the largest monitor this PC
// has to the atlas; from AddLauncherFonts, before the atlas is built.
void AddThemeFonts(ImFontAtlas* atlas);

// Text in the theme's type: `size` a line height at 1280x720, scaled with the
// page (Px). Caps text is uppercased here, so callers pass it as written.
struct TextStyle {
    FontWeight weight = FontWeight::kSemiBold;
    float size = text::kBody;
    bool caps = false;
};
inline constexpr TextStyle kCaps(float size) { return {FontWeight::kExtraBold, size, true}; }
inline constexpr TextStyle kPlain(float size) { return {FontWeight::kSemiBold, size, false}; }

// the text as it's drawn (uppercased for caps)
std::string Shown(const TextStyle& style, std::string_view text);
// how wide `text` draws, in pixels at the page's scale
float TextWidth(const TextStyle& style, std::string_view text);
// draws `text` with its line's top-left at `pos`, cut with an ellipsis if
// it's wider than `max_width` (0 for no limit); returns its width
float PaintText(ImDrawList* dl, ImVec2 pos, const TextStyle& style, ImU32 color,
               std::string_view text, float max_width = 0);
// `text` broken into lines no wider than `width`, at spaces
std::vector<std::string> WrapText(const TextStyle& style, std::string_view text, float width);

}
