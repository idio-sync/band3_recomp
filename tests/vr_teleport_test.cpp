// Checks src/Hooks/vr_teleport.h: which of BandCamShot::TeleportTarget's moves
// of the band vr_hold_band lets through, as a song's shots start and end with
// the hold off, on, and turned on or off mid-shot.

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include "src/Hooks/vr_teleport.h"

using band3::vr_view::TeleportGate;

namespace {

constexpr uint32_t kGuitar = 0x208C4AA4, kBass = 0x208C563C, kDrums = 0x208C61D4;

// a cut: the last shot puts its targets back, the next moves its own onto
// its marks; how many of those moves went ahead
int Cut(TeleportGate& g, bool hold, std::initializer_list<uint32_t> last,
        std::initializer_list<uint32_t> next) {
    int allowed = 0;
    for (uint32_t t : last) allowed += g.Allow(t, true, hold);
    for (uint32_t t : next) allowed += g.Allow(t, false, hold);
    return allowed;
}

}  // namespace

TEST_CASE("vr_hold_band off: every move onto a mark and back goes ahead") {
    TeleportGate g;
    CHECK(Cut(g, false, {}, {kGuitar, kBass, kDrums}) == 3);
    CHECK(g.Away() == 3);
    CHECK(Cut(g, false, {kGuitar, kBass, kDrums}, {kGuitar}) == 4);
    CHECK(g.Away() == 1);
    // a target put back that no shot moved (the song's first placing) goes
    // ahead too
    CHECK(g.Allow(kDrums, true, false));
}

TEST_CASE("vr_hold_band on from the song's start: the band never jumps at a cut") {
    TeleportGate g;
    for (int shot = 0; shot < 50; shot++)
        CHECK(Cut(g, true, {kGuitar, kBass, kDrums}, {kGuitar, kBass, kDrums}) == 0);
    CHECK(g.Away() == 0);
}

TEST_CASE("turned on mid-shot: a member the shot moved is put back as it ends, once") {
    TeleportGate g;
    // the shot began with the hold off: guitar and bass on their marks
    CHECK(Cut(g, false, {}, {kGuitar, kBass}) == 2);
    // held from here: that shot's end still puts them back...
    CHECK(Cut(g, true, {kGuitar, kBass}, {kGuitar, kDrums}) == 2);
    CHECK(g.Away() == 0);
    // ...and the next shot neither moves them nor puts them back
    CHECK(Cut(g, true, {kGuitar, kDrums}, {kBass}) == 0);
}

TEST_CASE("turned off mid-shot: the shot's end goes ahead, the next shot moves the band") {
    TeleportGate g;
    CHECK(Cut(g, true, {}, {kGuitar, kBass}) == 0);
    // its end puts them where the shot began (where they stood held: they
    // were never moved); the next shot's marks go ahead and are remembered
    CHECK(Cut(g, false, {kGuitar, kBass}, {kDrums}) == 3);
    CHECK(g.Away() == 1);
}

TEST_CASE("Reset forgets the members a shot moved, for the next song") {
    TeleportGate g;
    CHECK(Cut(g, false, {}, {kGuitar, kBass}) == 2);
    g.Reset();
    CHECK(g.Away() == 0);
    // another song's object at the same address, put back while held: skipped
    CHECK_FALSE(g.Allow(kGuitar, true, true));
}

namespace {

using band3::vr_view::kLeashFarRadius;
using band3::vr_view::kLeashFarSeconds;
using band3::vr_view::kLeashFarTurn;
using band3::vr_view::kLeashRadius;
using band3::vr_view::kLeashSeconds;
using band3::vr_view::kLeashTurn;
using band3::vr_view::LeashStep;

constexpr float kUnits = 39.37f;  // venue units a metre
constexpr float kFrame = 1.0f / 60;

float Dist(const float a[3], const float b[3]) { return std::hypot(a[0] - b[0], a[1] - b[1]); }

}  // namespace

TEST_CASE("the leash leaves a member alone near its place") {
    const float home[3] = {-70, 80, -179};
    float now[3] = {-70 + 0.4f * kUnits, 80, -179 + 50};
    const float was[3] = {now[0], now[1], now[2]};
    CHECK_FALSE(LeashStep(home, now, kFrame, kUnits));
    CHECK(now[0] == was[0]);
    CHECK(now[1] == was[1]);
    CHECK(now[2] == was[2]);
    // no time, no pull
    float far[3] = {home[0] + 1.0f * kUnits, home[1], home[2]};
    CHECK_FALSE(LeashStep(home, far, 0, kUnits));
}

TEST_CASE("past its radius, a member is pulled back a little a frame, toward its place") {
    const float home[3] = {10, 20, 0};
    float now[3] = {10, 20 + 1.0f * kUnits, 0};
    CHECK(LeashStep(home, now, kFrame, kUnits));
    // the excess (0.5 m) shrinks by a frame's share of kLeashSeconds: 5.6 mm
    const float excess = 1.0f - kLeashRadius;
    const float expect = kLeashRadius + excess * (1 - kFrame / kLeashSeconds);
    CHECK(Dist(home, now) / kUnits == doctest::Approx(expect).epsilon(1e-4));
    CHECK(now[0] == doctest::Approx(10));  // along the line home
    // 20 seconds at 60 frames a second take the excess back, and never pull it
    // inside the radius
    for (int f = 0; f < 1200; f++) LeashStep(home, now, kFrame, kUnits);
    CHECK(Dist(home, now) / kUnits == doctest::Approx(kLeashRadius).epsilon(1e-3));
    CHECK(Dist(home, now) / kUnits >= kLeashRadius - 1e-4f);
}

TEST_CASE("far out, a member is pulled back harder, but never at once") {
    const float home[3] = {0, 0, 170};
    float now[3] = {3.0f * kUnits, 0, 170 + 175};  // 3 m off, turned to -15 (175 degrees round)
    CHECK(LeashStep(home, now, kFrame, kUnits));
    // a frame: the 1.75 m past kLeashFarRadius shrinks by 1/24, the 0.75 m
    // between by 1/90 (8.1 cm in all)
    const float expect = kLeashRadius +
                         (kLeashFarRadius - kLeashRadius) * (1 - kFrame / kLeashSeconds) +
                         (3.0f - kLeashFarRadius) * (1 - kFrame / kLeashFarSeconds);
    CHECK(Dist(home, now) / kUnits == doctest::Approx(expect).epsilon(1e-4));
    CHECK(3.0f - Dist(home, now) / kUnits < 0.1f);
    // the turn measured the short way round (175 degrees counterclockwise),
    // pulled back a little on the same side
    const float turn = std::remainder(now[2] - home[2], 360.0f);
    CHECK(turn > 170);
    CHECK(turn < 175);
    // two seconds bring it inside the far radius and turn
    for (int f = 0; f < 120; f++) LeashStep(home, now, kFrame, kUnits);
    CHECK(Dist(home, now) / kUnits < kLeashFarRadius + 0.05f);
    CHECK(std::remainder(now[2] - home[2], 360.0f) < kLeashFarTurn + 5);
}

TEST_CASE("the heading is pulled back past kLeashTurn the short way round") {
    const float home[3] = {0, 0, -179};
    float now[3] = {0, 0, 101};  // 80 degrees clockwise of home, across the +-180 seam
    CHECK(LeashStep(home, now, kFrame, kUnits));
    const float off = std::remainder(now[2] - home[2], 360.0f);
    CHECK(off < 0);  // still clockwise of home
    CHECK(-off < 80);
    CHECK(-off > kLeashTurn);
}

TEST_CASE("walking away steadily, a member settles where the pull matches it") {
    // 1 m/s straight away from its place, for a minute: it settles about
    // 1.45 m out rather than walking off the stage, and no frame moves it
    // back more than the pull's share
    const float home[3] = {0, 0, 0};
    float now[3] = {0, 0, 0};
    float biggest_pull = 0;
    for (int f = 0; f < 3600; f++) {
        now[1] += 1.0f * kUnits * kFrame;
        const float before = now[1];
        LeashStep(home, now, kFrame, kUnits);
        biggest_pull = std::max(biggest_pull, before - now[1]);
    }
    CHECK(Dist(home, now) / kUnits == doctest::Approx(1.45f).epsilon(0.01));
    CHECK(biggest_pull / kUnits < 0.02f);  // a walk's step a frame, not a jump
}

TEST_CASE("a target moved twice before it's put back is put back once") {
    TeleportGate g;
    CHECK(g.Allow(kGuitar, false, false));
    CHECK(g.Allow(kGuitar, false, false));
    CHECK(g.Away() == 1);
    CHECK(g.Allow(kGuitar, true, true));
    CHECK_FALSE(g.Allow(kGuitar, true, true));
}
