#pragma once
#include <span>
#include <string_view>
#include <utility>
#include <vector>

// Which sizes of the launcher's two Montserrat weights to bake. The SDK's
// ImGui renderer builds its font texture once, when the drawer starts, and
// can't add a size later, so every size a screen may draw (theme.h's text
// sizes, at 1280x720) is baked up front for every scale the window can reach:
// up to the largest monitor's. Each size draws from the nearest bake at or
// above it (PickBake), so the bakes step up a quarter at a time.
//
// The large caps (titles, the hub's menu) would make the texture huge with
// every Latin letter at every size, so above the full sets' top they're baked
// with the capitals, digits and a little punctuation only (kTitleGlyphs).

namespace band3::launcher::ui {

enum class FontWeight { kSemiBold, kExtraBold };

struct FontBake {
    FontWeight weight = FontWeight::kSemiBold;
    float px = 0;
    // the capitals, digits and title punctuation only
    bool titles_only = false;
};

// the characters a titles-only bake has (UTF-8 free: ASCII only)
inline constexpr const char* kTitleGlyphs = " !&'()+,-./0123456789:?ABCDEFGHIJKLMNOPQRSTUVWXYZ";
// a full bake's characters, first-last pairs: Latin, Latin-1, Latin
// Extended-A, and punctuation (dashes, quotes, the ellipsis, primes)
inline constexpr unsigned short kFullRanges[] = {0x0020, 0x007E, 0x00A0, 0x017F,
                                                 0x2010, 0x2027, 0x2030, 0x203A};

// the largest text each kind of bake has to draw at 1280x720; larger caps
// (box titles, panel headings) are band3's own words, in the titles' bakes
inline constexpr float kLargestSemiBold = 28;
inline constexpr float kLargestExtraBold = 26;
inline constexpr float kLargestTitle = 96;
// the smallest text, at the smallest scale (ScaleFor's floor)
inline constexpr float kSmallestText = 16 * 0.75f;
// each bake's size over the one before; the titles', large and faded, step
// further, since drawing them up to half again smaller doesn't show
inline constexpr float kBakeStep = 1.25f;
inline constexpr float kTitleStep = 1.5f;

// The bakes for a window drawn at most `max_scale` times 1280x720: SemiBold
// and ExtraBold with every Latin letter up to their largest size at that
// scale, then ExtraBold titles-only up to kLargestTitle at that scale.
std::vector<FontBake> PlanFontBakes(float max_scale);

// the bake to draw `px` of `weight` with: the smallest at or above it, or the
// largest there is. Titles-only bakes only when `title` (every character of
// the text is in kTitleGlyphs; IsTitleText); nullptr when there's no bake of
// that weight.
const FontBake* PickBake(std::span<const FontBake> bakes, FontWeight weight, float px, bool title);

// whether a titles-only bake has every character of `text`
bool IsTitleText(std::string_view text);

// The scale the largest of these monitors (width, height) draws the launcher
// at full screen: min(width / 1280, height / 720), within ScaleFor's 0.75 to
// 3. kUnknownScale when there are none (Linux can't list them yet).
inline constexpr float kUnknownScale = 2.0f;
float MaxScaleFor(std::span<const std::pair<int, int>> monitors);

}
