// Checks the redesigned launcher's pieces that need neither ImGui nor the SDK
// (src/Launcher/ui/): which font sizes are baked, the navigation actions, and
// the RB3 textures read from the game data.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>
#include "src/Launcher/ui/font_plan.h"
#include "src/Launcher/ui/navigation.h"
#include "src/Launcher/ui/skin_images.h"
#include "src/Input/instruments.h"

namespace fs = std::filesystem;
using namespace band3::launcher;
using namespace band3::launcher::ui;
namespace xbox = band3::input::xbox;
namespace http = band3::http;

namespace {

float Largest(const std::vector<FontBake>& bakes, FontWeight weight, bool titles_only) {
    float largest = 0;
    for (const FontBake& b : bakes) {
        if (b.weight == weight && b.titles_only == titles_only) largest = std::max(largest, b.px);
    }
    return largest;
}

// the texture area the bakes need, in glyph cells of their size squared
double Area(const std::vector<FontBake>& bakes) {
    double full = 0;
    for (size_t i = 0; i + 1 < std::size(kFullRanges); i += 2) full += kFullRanges[i + 1] - kFullRanges[i] + 1;
    const double titles = static_cast<double>(std::strlen(kTitleGlyphs));
    double area = 0;
    for (const FontBake& b : bakes) area += double(b.px) * b.px * (b.titles_only ? titles : full);
    return area;
}

}

TEST_CASE("the font bakes reach every size a 720p window draws") {
    const std::vector<FontBake> bakes = PlanFontBakes(1.0f);
    REQUIRE_FALSE(bakes.empty());
    CHECK(Largest(bakes, FontWeight::kSemiBold, false) >= kLargestSemiBold);
    CHECK(Largest(bakes, FontWeight::kExtraBold, false) >= kLargestExtraBold);
    CHECK(Largest(bakes, FontWeight::kExtraBold, true) >= kLargestTitle);
    // titles-only bakes are ExtraBold, and only above the full sets
    for (const FontBake& b : bakes) {
        if (!b.titles_only) continue;
        CHECK(b.weight == FontWeight::kExtraBold);
        CHECK(b.px > Largest(bakes, FontWeight::kExtraBold, false));
    }
}

TEST_CASE("the font bakes step up a quarter at most from the smallest text, titles a half") {
    const std::vector<FontBake> bakes = PlanFontBakes(2.0f);
    for (FontWeight weight : {FontWeight::kSemiBold, FontWeight::kExtraBold}) {
        // the full sets, then the titles from the full set's top
        std::vector<float> sizes;
        for (const FontBake& b : bakes) {
            if (b.weight == weight && !b.titles_only) sizes.push_back(b.px);
        }
        std::sort(sizes.begin(), sizes.end());
        REQUIRE(sizes.size() > 2);
        CHECK(sizes.front() <= kSmallestText);
        for (size_t i = 1; i < sizes.size(); i++) CHECK(sizes[i] <= sizes[i - 1] * kBakeStep + 0.5f);
        std::vector<float> titles{sizes.back()};
        for (const FontBake& b : bakes) {
            if (b.weight == weight && b.titles_only) titles.push_back(b.px);
        }
        std::sort(titles.begin(), titles.end());
        for (size_t i = 1; i < titles.size(); i++) CHECK(titles[i] <= titles[i - 1] * kTitleStep + 0.5f);
    }
}

TEST_CASE("title text is caps, digits and a little punctuation") {
    CHECK(IsTitleText("SETTINGS"));
    CHECK(IsTitleText("FIND ROCK BAND 3"));
    CHECK(IsTitleText("1,612 SONGS - 412 WITH VIDEOS"));
    CHECK_FALSE(IsTitleText("Settings"));
    CHECK_FALSE(IsTitleText("LOOKING IN D:\\GAMES"));
    CHECK_FALSE(IsTitleText("MOT\xC3\x96RHEAD"));
    CHECK(IsTitleText(""));
}

TEST_CASE("a 4K monitor's bakes reach its sizes and still fit a texture") {
    const std::vector<FontBake> bakes = PlanFontBakes(3.0f);
    CHECK(Largest(bakes, FontWeight::kSemiBold, false) >= kLargestSemiBold * 3);
    CHECK(Largest(bakes, FontWeight::kExtraBold, true) >= kLargestTitle * 3);
    // about 24 megapixels: an 4096-wide atlas under 8192 rows
    CHECK(Area(bakes) < 24e6);
    // a 1080p monitor's are far smaller
    CHECK(Area(PlanFontBakes(1.5f)) < Area(bakes) / 3);
}

TEST_CASE("text draws from the smallest bake at or above its size") {
    const std::vector<FontBake> bakes = {
        {FontWeight::kSemiBold, 20, false},  {FontWeight::kSemiBold, 25, false},
        {FontWeight::kExtraBold, 20, false}, {FontWeight::kExtraBold, 25, false},
        {FontWeight::kExtraBold, 40, true},
    };
    const FontBake* b = PickBake(bakes, FontWeight::kSemiBold, 21, false);
    REQUIRE(b);
    CHECK(b->px == 25);
    CHECK(PickBake(bakes, FontWeight::kSemiBold, 20, false)->px == 20);
    // past the largest: the largest
    CHECK(PickBake(bakes, FontWeight::kSemiBold, 60, false)->px == 25);
    // titles-only bakes only for titles
    CHECK(PickBake(bakes, FontWeight::kExtraBold, 30, false)->px == 25);
    CHECK(PickBake(bakes, FontWeight::kExtraBold, 30, true)->px == 40);
    CHECK(PickBake(bakes, FontWeight::kExtraBold, 22, true)->px == 25);
    CHECK(PickBake({}, FontWeight::kSemiBold, 20, false) == nullptr);
}

TEST_CASE("the largest monitor sets the scale the fonts are baked for") {
    using M = std::pair<int, int>;
    CHECK(MaxScaleFor(std::vector<M>{{1920, 1080}}) == doctest::Approx(1.5));
    CHECK(MaxScaleFor(std::vector<M>{{1920, 1080}, {3840, 2160}}) == doctest::Approx(3.0));
    // an ultrawide: its height decides
    CHECK(MaxScaleFor(std::vector<M>{{3440, 1440}}) == doctest::Approx(2.0));
    // within ScaleFor's range
    CHECK(MaxScaleFor(std::vector<M>{{7680, 4320}}) == doctest::Approx(3.0));
    CHECK(MaxScaleFor(std::vector<M>{{800, 600}}) == doctest::Approx(0.75));
    CHECK(MaxScaleFor({}) == doctest::Approx(kUnknownScale));
}

TEST_CASE("the selection moves and stops or wraps at the ends") {
    CHECK(MoveSelection(0, 1, 3, false) == 1);
    CHECK(MoveSelection(2, 1, 3, false) == 2);
    CHECK(MoveSelection(0, -1, 3, false) == 0);
    CHECK(MoveSelection(2, 1, 3, true) == 0);
    CHECK(MoveSelection(0, -1, 3, true) == 2);
}

TEST_CASE("the selection skips what can't take focus") {
    const auto odd = [](int i) { return i % 2 == 1; };
    CHECK(MoveSelection(1, 1, 5, false, odd) == 3);
    // nothing focusable past 3: it stays
    CHECK(MoveSelection(3, 1, 5, false, odd) == 3);
    CHECK(MoveSelection(3, 1, 5, true, odd) == 1);
    CHECK(MoveSelection(1, -1, 5, true, odd) == 3);
    CHECK(MoveSelection(0, 1, 0, false) == -1);
    CHECK(MoveSelection(2, 1, 4, false, [](int) { return false; }) == 2);
    CHECK(FirstFocusable(5, odd) == 1);
    CHECK(FirstFocusable(5) == 0);
    CHECK(FirstFocusable(5, [](int) { return false; }) == -1);
    CHECK(FirstFocusable(0) == -1);
}

TEST_CASE("a held direction fires, waits, then repeats") {
    RepeatGate gate;
    CHECK(gate.Step(true, 0.0));
    CHECK_FALSE(gate.Step(true, 0.1));
    CHECK_FALSE(gate.Step(true, 0.37));
    CHECK(gate.Step(true, 0.39));
    CHECK_FALSE(gate.Step(true, 0.42));
    CHECK(gate.Step(true, 0.47));
    CHECK_FALSE(gate.Step(false, 0.5));
    // pressed again: at once
    CHECK(gate.Step(true, 0.51));
}

TEST_CASE("a held button fires once it has been held long enough") {
    HoldGate gate;
    CHECK_FALSE(gate.Step(true, 0.0));
    CHECK(gate.Progress(0.5) == doctest::Approx(0.5));
    CHECK_FALSE(gate.Step(true, 0.9));
    CHECK(gate.Step(true, 1.01));
    // once only
    CHECK_FALSE(gate.Step(true, 2.0));
    CHECK_FALSE(gate.Step(false, 2.1));
    CHECK(gate.Progress(2.1) == 0);
    // let go early: nothing
    CHECK_FALSE(gate.Step(true, 3.0));
    CHECK_FALSE(gate.Step(false, 3.5));
    CHECK_FALSE(gate.Step(true, 3.6));
}

TEST_CASE("a controller's buttons become the launcher's actions") {
    PadActions pad;
    const auto press = [&](uint16_t buttons, double now) {
        NavPad p;
        p.buttons = buttons;
        return pad.Step(p, now);
    };
    // held at the first step: not pressed
    CHECK(press(xbox::kButtonA, 0.0).empty());
    CHECK(press(0, 0.1).empty());
    CHECK(press(xbox::kButtonA, 0.2) == std::vector<Action>{Action::kSelect});
    CHECK(press(xbox::kButtonA, 0.3).empty());
    CHECK(press(xbox::kButtonB, 0.4) == std::vector<Action>{Action::kBack});
    CHECK(press(xbox::kBack, 0.5) == std::vector<Action>{Action::kBack});
    CHECK(press(xbox::kButtonX, 0.6) == std::vector<Action>{Action::kSecondary});
    CHECK(press(xbox::kButtonY, 0.7) == std::vector<Action>{Action::kReset});
    CHECK(press(xbox::kLeftShoulder, 0.8) == std::vector<Action>{Action::kPreviousCategory});
    CHECK(press(xbox::kRightShoulder, 0.9) == std::vector<Action>{Action::kNextCategory});
    CHECK(press(xbox::kStart, 1.0) == std::vector<Action>{Action::kPlay});
    CHECK(press(0, 1.1).empty());
    // the D-pad repeats
    CHECK(press(xbox::kDpadDown, 2.0) == std::vector<Action>{Action::kDown});
    CHECK(press(xbox::kDpadDown, 2.1).empty());
    CHECK(press(xbox::kDpadDown, 2.4) == std::vector<Action>{Action::kDown});
}

TEST_CASE("the left stick moves like the D-pad") {
    PadActions pad;
    NavPad p;
    CHECK(pad.Step(p, 0.0).empty());
    p.left_y = -0.8f;
    CHECK(pad.Step(p, 0.1) == std::vector<Action>{Action::kDown});
    p.left_y = 0.2f;  // within the dead zone
    CHECK(pad.Step(p, 0.2).empty());
    p.left_x = 0.9f;
    CHECK(pad.Step(p, 0.3) == std::vector<Action>{Action::kRight});
}

TEST_CASE("holding Start a second is a held Start") {
    PadActions pad;
    NavPad p;
    pad.Step(p, 0.0);
    p.buttons = xbox::kStart;
    pad.Step(p, 0.1);
    CHECK_FALSE(pad.HeldStart());
    pad.Step(p, 0.6);
    CHECK_FALSE(pad.HeldStart());
    pad.Step(p, 1.2);
    CHECK(pad.HeldStart());
}

TEST_CASE("every part of the skin has somewhere to come from") {
    for (size_t i = 0; i < kSkinPartCount; i++) {
        bool found = false;
        for (const SkinSource& s : SkinSources()) found = found || static_cast<size_t>(s.part) == i;
        CHECK_MESSAGE(found, "part ", i);
    }
}

TEST_CASE("without game data the skin is empty and nothing fails") {
    const fs::path root = fs::temp_directory_path() / "band3_skin_missing";
    fs::remove_all(root);
    const GameFiles files(root);
    CHECK_FALSE(files.HasArchives());
    CHECK_FALSE(files.Read("ui/song_select/gen/song_select.milo_xbox").has_value());
    for (const auto& image : LoadSkinImages(files)) CHECK_FALSE(image.has_value());
}

TEST_CASE("a loose file is read before the archives") {
    const fs::path root = fs::temp_directory_path() / "band3_skin_loose";
    fs::remove_all(root);
    fs::create_directories(root / "ui" / "gen");
    std::ofstream(root / "ui" / "gen" / "a.txt", std::ios::binary) << "loose";
    const GameFiles files(root);
    const auto read = files.Read("ui/gen/a.txt");
    REQUIRE(read.has_value());
    CHECK(*read == "loose");
    fs::remove_all(root);
}

TEST_CASE("the streaks are clear where the normal map is flat") {
    http::Image flat{4, 2, std::vector<uint8_t>(4 * 2 * 4, 128)};
    const http::Image streaks = MakeStreaks(flat);
    CHECK(streaks.width == 16);
    CHECK(streaks.height == 2);
    REQUIRE(streaks.rgba.size() == 16 * 2 * 4);
    for (size_t i = 3; i < streaks.rgba.size(); i += 4) CHECK(streaks.rgba[i] == 0);
}

TEST_CASE("the streaks are blue where the normal map faces up") {
    http::Image up{8, 4, std::vector<uint8_t>(8 * 4 * 4, 128)};
    for (size_t i = 1; i < up.rgba.size(); i += 4) up.rgba[i] = 255;  // green: facing up
    const http::Image streaks = MakeStreaks(up);
    const size_t mid = (size_t(2) * streaks.width + streaks.width / 2) * 4;
    CHECK(streaks.rgba[mid + 3] > 200);
    CHECK(streaks.rgba[mid + 2] > streaks.rgba[mid + 0]);
    CHECK(streaks.rgba[mid + 2] > streaks.rgba[mid + 1]);
}

TEST_CASE("the skin reads from the game data in assets" * doctest::skip()) {
    const GameFiles files(fs::path(BAND3_ROOT_DIR) / "assets");
    REQUIRE(files.HasArchives());
    const SkinImages images = LoadSkinImages(files);
    for (SkinPart part : {SkinPart::kHighlight, SkinPart::kStrip, SkinPart::kFrame, SkinPart::kFill,
                          SkinPart::kLineFade, SkinPart::kGrain, SkinPart::kIcons}) {
        CHECK_MESSAGE(images[static_cast<size_t>(part)].has_value(), "part ", int(part));
    }
}
