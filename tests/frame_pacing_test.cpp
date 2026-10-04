// Checks src/Hooks/frame_pacing.h: the world's period under even/odd
// rendering matches RB3's own at 60 Hz, keeps the venue's rate at higher
// refresh rates, and follows background_fps when it's set; and the frame cap
// keeps its beat, starts it again after a hitch, and reads frame_cap.

#include <doctest/doctest.h>
#include "src/Hooks/frame_pacing.h"

using band3::pacing::WorldHalfFrames;

TEST_CASE("at 60 Hz the period is RB3's own") {
    CHECK(WorldHalfFrames(60, 30, 0) == 4);  // every other frame
    CHECK(WorldHalfFrames(60, 24, 0) == 5);  // 2 and 3 frames by turns
    CHECK(WorldHalfFrames(60, 20, 0) == 6);
    CHECK(WorldHalfFrames(60, 60, 0) == 2);  // every frame
}

TEST_CASE("a higher refresh rate keeps the venue's rate") {
    CHECK(WorldHalfFrames(120, 30, 0) == 8);
    CHECK(WorldHalfFrames(180, 30, 0) == 12);
    CHECK(WorldHalfFrames(240, 30, 0) == 16);
    // 4.8 frames: rounds to 5 (28.8 fps)
    CHECK(WorldHalfFrames(144, 30, 0) == 10);
}

TEST_CASE("background_fps overrides the venue's rate") {
    CHECK(WorldHalfFrames(180, 30, 60) == 6);
    CHECK(WorldHalfFrames(60, 30, 20) == 6);
    CHECK(WorldHalfFrames(120, 30, 120) == 2);
    // no faster than the game itself
    CHECK(WorldHalfFrames(120, 30, 240) == 2);
}

TEST_CASE("a venue that asks for no rate is left drawing every frame") {
    CHECK(WorldHalfFrames(120, 0, 0) == 0);
    CHECK(WorldHalfFrames(120, 0, 30) == 0);
}

TEST_CASE("a venue's rate is kept to SetEmulateFPS's 1..60") {
    CHECK(WorldHalfFrames(120, 90, 0) == 4);
    CHECK(WorldHalfFrames(60, 1, 0) == 120);
}

TEST_CASE("an unset refresh rate is the console's 60") {
    CHECK(WorldHalfFrames(0, 30, 0) == 4);
}

TEST_CASE("the longest period counts ProcCounter's odd half-frame") {
    using band3::pacing::MaxPeriod;
    CHECK(MaxPeriod(2, 0) == 2);
    CHECK(MaxPeriod(2, 1) == 3);   // 2 now, then 3
    CHECK(MaxPeriod(3, -1) == 3);  // 3 now, then 2
    CHECK(MaxPeriod(0, 0) == 0);   // every frame
}

using band3::pacing::FrameCap;
using band3::pacing::FrameCapMode;
using band3::pacing::FrameCapSchedule;
using band3::pacing::FrameCapSetting;
using band3::pacing::ParseFrameCap;
using band3::pacing::ResolveFrameCap;

namespace {

constexpr int64_t kPeriod = 8'333'333;  // 120 Hz
constexpr int64_t kStart = 1'000'000'000;

FrameCapSchedule Started() {
    FrameCapSchedule s;
    s.SetPeriod(kPeriod);
    // the first frame starts the beat and doesn't wait
    CHECK(s.Next(kStart) == kStart);
    return s;
}

}

TEST_CASE("the cap's beats are whole periods from the first") {
    FrameCapSchedule s = Started();
    // frames that end at odd times early in their beat all wait for it
    const int64_t ends[] = {1'000'000, 7'000'000, 3'500'000, 8'000'000, 100};
    for (int i = 1; i <= 5; i++) {
        const int64_t now = kStart + (i - 1) * kPeriod + ends[i - 1];
        CHECK(s.Next(now) == kStart + i * kPeriod);
    }
    CHECK(s.Late() == 0);
    CHECK(s.Resets() == 0);
}

TEST_CASE("a frame that ends early waits to its beat") {
    FrameCapSchedule s = Started();
    CHECK(s.Next(kStart + 2'000'000) == kStart + kPeriod);
    // ending exactly on the beat waits no longer
    CHECK(s.Next(kStart + 2 * kPeriod) == kStart + 2 * kPeriod);
}

TEST_CASE("a frame late by less than a period goes on at once and keeps the beat") {
    FrameCapSchedule s = Started();
    const int64_t late = kStart + kPeriod + 3'000'000;
    CHECK(s.Next(late) == late);
    CHECK(s.Late() == 1);
    // the next frame's wait is shorter, to the same beat
    CHECK(s.Next(late + 1'000'000) == kStart + 2 * kPeriod);
    CHECK(s.Resets() == 0);
}

TEST_CASE("a frame a period or more late starts the beat again from itself") {
    FrameCapSchedule s = Started();
    // a hitch: the beat at kStart + kPeriod missed by a whole period
    const int64_t hitch = kStart + 2 * kPeriod;
    CHECK(s.Next(hitch) == hitch);
    CHECK(s.Resets() == 1);
    CHECK(s.Late() == 0);
    // no burst to catch up: the next frame waits a period from the hitch
    CHECK(s.Next(hitch + 1'000'000) == hitch + kPeriod);
    CHECK(s.Next(hitch + kPeriod + 1'000'000) == hitch + 2 * kPeriod);
    // a render check holding the game for seconds counts the same
    const int64_t held = hitch + 5'000'000'000;
    CHECK(s.Next(held) == held);
    CHECK(s.Resets() == 2);
    CHECK(s.Next(held + 10) == held + kPeriod);
}

TEST_CASE("a new period or turning the cap off starts the beat again") {
    FrameCapSchedule s = Started();
    s.SetPeriod(kPeriod);  // the same period keeps the beat
    CHECK(s.Next(kStart + 1'000'000) == kStart + kPeriod);

    // another period: the next frame starts a beat of its own
    s.SetPeriod(16'666'667);
    const int64_t now = kStart + kPeriod + 500'000;
    CHECK(s.Next(now) == now);
    CHECK(s.Next(now + 1'000'000) == now + 16'666'667);

    // off: every frame goes on at once
    s.SetPeriod(0);
    CHECK(s.Next(now + 2'000'000) == now + 2'000'000);
    CHECK(s.Next(now + 2'000'001) == now + 2'000'001);
    // and on again starts afresh, without counting the time off as a hitch
    s.SetPeriod(kPeriod);
    const int64_t later = now + 9'000'000'000;
    CHECK(s.Next(later) == later);
    CHECK(s.Next(later + 1) == later + kPeriod);
    CHECK(s.Resets() == 0);
}

TEST_CASE("frame_cap's values") {
    CHECK(ParseFrameCap("display")->mode == FrameCapMode::kDisplay);
    CHECK(ParseFrameCap("auto")->mode == FrameCapMode::kAuto);
    CHECK(ParseFrameCap("off")->mode == FrameCapMode::kOff);
    const auto fixed = ParseFrameCap("117");
    REQUIRE(fixed);
    CHECK(fixed->mode == FrameCapMode::kFixed);
    CHECK(fixed->hz == 117);
    CHECK(ParseFrameCap("59.94")->hz == doctest::Approx(59.94));
    // a number of Hz is kept to 24..240
    CHECK(ParseFrameCap("10")->hz == 24);
    CHECK(ParseFrameCap("1000")->hz == 240);
    for (const char* bad : {"", "Display", "on", "0", "-60", "120hz", " 120", "nan", "inf"}) {
        CAPTURE(bad);
        CHECK_FALSE(ParseFrameCap(bad));
    }
}

TEST_CASE("display caps at the display's refresh rate exactly") {
    const FrameCapSetting display{FrameCapMode::kDisplay};
    const FrameCap at_119 = ResolveFrameCap(display, 120000, 1001);
    CHECK(at_119.mode == FrameCapMode::kDisplay);
    CHECK(at_119.hz == doctest::Approx(119.88).epsilon(1e-4));
    // 8.3417 ms, from the fraction
    CHECK(at_119.period_ns == 8'341'667);
    CHECK(ResolveFrameCap(display, 60, 1).period_ns == 16'666'667);
    CHECK(ResolveFrameCap(display, 144, 1).hz == 144);
}

TEST_CASE("auto caps a little under the display's rate") {
    const FrameCapSetting autocap{FrameCapMode::kAuto};
    // at least 4 fps under
    CHECK(ResolveFrameCap(autocap, 60, 1).hz == doctest::Approx(56));
    // 5% under, once that's more
    CHECK(ResolveFrameCap(autocap, 120, 1).hz == doctest::Approx(114));
    CHECK(ResolveFrameCap(autocap, 144, 1).hz == doctest::Approx(136.8));
    CHECK(ResolveFrameCap(autocap, 144, 1).period_ns == std::llround(1e9 / 136.8));
}

TEST_CASE("a number of Hz caps there, whatever the display") {
    const FrameCapSetting fixed{FrameCapMode::kFixed, 117};
    const FrameCap with = ResolveFrameCap(fixed, 144, 1);
    CHECK(with.mode == FrameCapMode::kFixed);
    CHECK(with.hz == 117);
    CHECK(with.period_ns == std::llround(1e9 / 117));
    CHECK(ResolveFrameCap(fixed, 0, 0).period_ns == with.period_ns);
}

TEST_CASE("without the display's rate, display and auto are off") {
    for (FrameCapMode mode : {FrameCapMode::kDisplay, FrameCapMode::kAuto}) {
        const FrameCap cap = ResolveFrameCap({mode}, 0, 0);
        CHECK(cap.mode == FrameCapMode::kOff);
        CHECK(cap.period_ns == 0);
        CHECK(cap.hz == 0);
        CHECK(ResolveFrameCap({mode}, 120, 0).period_ns == 0);
    }
    // and off is off with one
    CHECK(ResolveFrameCap({FrameCapMode::kOff}, 120, 1).period_ns == 0);
}
