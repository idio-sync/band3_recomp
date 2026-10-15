#include "font_plan.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace band3::launcher::ui {

namespace {

// whole-pixel bakes from `from` up by at most `step` each to `to`, the last
// one exactly `to` (rounded up)
void Ladder(std::vector<FontBake>& out, FontWeight weight, bool titles_only, float from, float to,
            float step) {
    const float top = std::ceil(to);
    for (float px = std::round(from);; px = std::max(px + 1, std::floor(px * step))) {
        if (px >= top) {
            out.push_back({weight, top, titles_only});
            return;
        }
        out.push_back({weight, px, titles_only});
    }
}

}

std::vector<FontBake> PlanFontBakes(float max_scale) {
    const float s = std::clamp(max_scale, 0.75f, 3.0f);
    std::vector<FontBake> bakes;
    Ladder(bakes, FontWeight::kSemiBold, false, kSmallestText, kLargestSemiBold * s, kBakeStep);
    Ladder(bakes, FontWeight::kExtraBold, false, kSmallestText, kLargestExtraBold * s, kBakeStep);
    // the titles down from their largest, so none is wasted just under it
    const float full_top = bakes.back().px;
    std::vector<FontBake> titles;
    for (float px = std::ceil(kLargestTitle * s); px > full_top; px = std::ceil(px / kTitleStep)) {
        titles.push_back({FontWeight::kExtraBold, px, true});
    }
    bakes.insert(bakes.end(), titles.rbegin(), titles.rend());
    return bakes;
}

const FontBake* PickBake(std::span<const FontBake> bakes, FontWeight weight, float px, bool title) {
    const FontBake* above = nullptr;
    const FontBake* largest = nullptr;
    for (const FontBake& b : bakes) {
        if (b.weight != weight || (b.titles_only && !title)) continue;
        if (!largest || b.px > largest->px) largest = &b;
        if (b.px >= px && (!above || b.px < above->px)) above = &b;
    }
    return above ? above : largest;
}

bool IsTitleText(std::string_view text) {
    return std::all_of(text.begin(), text.end(),
                       [](char c) { return c != '\0' && std::strchr(kTitleGlyphs, c) != nullptr; });
}

float MaxScaleFor(std::span<const std::pair<int, int>> monitors) {
    if (monitors.empty()) return kUnknownScale;
    float scale = 0;
    for (const auto& [w, h] : monitors) {
        scale = std::max(scale, std::min(w / 1280.0f, h / 720.0f));
    }
    return std::clamp(scale, 0.75f, 3.0f);
}

}
