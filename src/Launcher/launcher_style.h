#pragma once
#include <imgui.h>

// The launcher's look: its colours, its font at a few sizes, and the scale
// the page is drawn at, from the window's size. Everything is sized for a
// 1280x720 window and scaled from there (Px).

namespace band3::launcher {

// Colours

inline constexpr ImVec4 kBackground(0.071f, 0.075f, 0.090f, 1.0f);
inline constexpr ImVec4 kPopup(0.105f, 0.110f, 0.135f, 1.0f);
inline constexpr ImVec4 kFrame(0.150f, 0.157f, 0.190f, 1.0f);
inline constexpr ImVec4 kFrameHover(0.200f, 0.208f, 0.250f, 1.0f);
inline constexpr ImVec4 kFrameActive(0.245f, 0.255f, 0.305f, 1.0f);
inline constexpr ImVec4 kLine(0.200f, 0.208f, 0.245f, 1.0f);
inline constexpr ImVec4 kText(0.925f, 0.925f, 0.945f, 1.0f);
inline constexpr ImVec4 kMuted(0.585f, 0.600f, 0.660f, 1.0f);
inline constexpr ImVec4 kAccent(0.937f, 0.435f, 0.235f, 1.0f);
inline constexpr ImVec4 kAccentHover(1.000f, 0.540f, 0.330f, 1.0f);
inline constexpr ImVec4 kAccentActive(0.820f, 0.355f, 0.180f, 1.0f);
inline constexpr ImVec4 kAccentDim(0.380f, 0.200f, 0.130f, 1.0f);
inline constexpr ImVec4 kGood(0.470f, 0.840f, 0.500f, 1.0f);
inline constexpr ImVec4 kWarn(1.000f, 0.700f, 0.330f, 1.0f);
inline constexpr ImVec4 kBad(1.000f, 0.470f, 0.420f, 1.0f);
inline constexpr ImVec4 kDeckBanner(0.105f, 0.150f, 0.215f, 1.0f);
inline constexpr ImVec4 kProblemBanner(0.235f, 0.125f, 0.090f, 1.0f);

// Fonts and scale

// text sizes at a 1280x720 window
inline constexpr float kBodySize = 20;
inline constexpr float kSmallSize = 17;
inline constexpr float kHeadingSize = 23;
inline constexpr float kTitleSize = 30;

// Adds the launcher's fonts: a readable UI face at a few sizes, so the page
// stays sharp from 720p to 4K. From Band3App::OnConfigureFonts, which runs
// before band3 knows whether the launcher will show. Uses a system font when
// one is there and the SDK's own font, larger, otherwise.
void AddLauncherFonts(ImFontAtlas* atlas);

// how much bigger than at 1280x720 the page is drawn in a window this size
float ScaleFor(const ImVec2& display);
// the page's scale, set at the start of each frame (LauncherDialog::OnDraw)
void SetScale(float scale);
float Scale();
// a length at 1280x720, at the page's scale
inline float Px(float v) { return v * Scale(); }

// the launcher's look, over the SDK's style for the length of its draw
void ApplyStyle(ImGuiStyle& style, float scale);

// a section's heading, in the accent colour over a hairline across the page
void SectionHeading(const char* name);

// a button's width, for lining up a row of them on the right
inline float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

// a box across the page: Steam Deck, band3.toml problems
template <typename Body>
void Banner(const char* id, const ImVec4& background, Body&& body) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(10)));
    if (ImGui::BeginChild(id, ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar)) {
        body();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

// the launcher's font at a 720p size, scaled with the page
class FontScope {
public:
    explicit FontScope(float size);
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

}
