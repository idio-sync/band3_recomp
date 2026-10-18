// Checks src/Hooks/frame_pacing.h: the world's period under even/odd
// rendering matches RB3's own at 60 Hz, keeps the venue's rate at higher
// refresh rates, and follows background_fps when it's set (or a faster music
// video's rate); and the frame cap
// keeps its beat, starts it again after a hitch, and reads frame_cap.

#include <doctest/doctest.h>
#include <string>
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

TEST_CASE("a music video faster than the background has it drawn at the video's rate") {
    using band3::pacing::BackgroundFpsForVideo;
    // 60 fps (and 59.94) in a 30 fps venue: every frame at 60 Hz
    CHECK(BackgroundFpsForVideo(30, 0, 60) == 60);
    CHECK(BackgroundFpsForVideo(30, 0, 59.94) == 60);
    CHECK(WorldHalfFrames(60, 30, BackgroundFpsForVideo(30, 0, 59.94)) == 2);
    // at 120 Hz, every other frame
    CHECK(WorldHalfFrames(120, 30, BackgroundFpsForVideo(30, 0, 60)) == 4);
    // no faster than background_fps already draws it
    CHECK(BackgroundFpsForVideo(30, 90, 60) == 90);
    CHECK(BackgroundFpsForVideo(30, 20, 60) == 60);
    // a 30, 29.97 or 24 fps video leaves the venue's rate alone
    CHECK(BackgroundFpsForVideo(30, 0, 30) == 0);
    CHECK(BackgroundFpsForVideo(30, 0, 29.97) == 0);
    CHECK(BackgroundFpsForVideo(30, 0, 23.976) == 0);
    CHECK(BackgroundFpsForVideo(30, 45, 24) == 45);
    // none playing, or a venue drawing every frame already
    CHECK(BackgroundFpsForVideo(30, 0, 0) == 0);
    CHECK(BackgroundFpsForVideo(0, 0, 60) == 0);
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

TEST_CASE("a frame a period or more late goes on at once and keeps the beat's phase") {
    FrameCapSchedule s = Started();
    // a hitch: the beat at kStart + kPeriod missed by more than a whole period
    const int64_t hitch = kStart + 2 * kPeriod + 3'000'000;
    CHECK(s.Next(hitch) == hitch);
    CHECK(s.Resets() == 1);
    CHECK(s.Late() == 0);
    // the next frame waits for the first beat after the hitch, on the old
    // grid, not a period from the hitch
    CHECK(s.Next(hitch + 1'000'000) == kStart + 3 * kPeriod);
    // no burst to catch up: the beats after follow a period apart
    CHECK(s.Next(kStart + 3 * kPeriod + 1'000'000) == kStart + 4 * kPeriod);
    CHECK(s.Next(kStart + 4 * kPeriod + 500'000) == kStart + 5 * kPeriod);
    CHECK(s.Late() == 0);
    // a render check holding the game for seconds counts the same, and the
    // phase is still the first frame's
    const int64_t held = kStart + 5 * kPeriod + 5'000'000'000;
    CHECK(s.Next(held) == held);
    CHECK(s.Resets() == 2);
    const int64_t beat = kStart + ((held - kStart) / kPeriod + 1) * kPeriod;
    CHECK(beat > held);
    CHECK(beat - held <= kPeriod);
    CHECK(s.Next(held + 10) == beat);
    CHECK(s.Next(beat + 2'000'000) == beat + kPeriod);
}

TEST_CASE("a frame late by exactly whole periods waits a whole period for the next beat") {
    FrameCapSchedule s = Started();
    // ends right on a beat it didn't wait for: that beat is the frame's own,
    // so the next frame's is the one after
    const int64_t on_beat = kStart + 3 * kPeriod;
    CHECK(s.Next(on_beat) == on_beat);
    CHECK(s.Resets() == 1);
    CHECK(s.Next(on_beat + 1'000'000) == on_beat + kPeriod);
}

TEST_CASE("the phase kept across a hitch includes the nudges before it") {
    FrameCapSchedule s = Started();
    s.Nudge(-1'000'000);
    CHECK(s.Next(kStart + 1'000'000) == kStart + kPeriod - 1'000'000);
    const int64_t hitch = kStart + 4 * kPeriod;
    CHECK(s.Next(hitch) == hitch);
    CHECK(s.Resets() == 1);
    CHECK(s.Next(hitch + 1'000'000) == kStart + 5 * kPeriod - 1'000'000);
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

TEST_CASE("a nudge moves the beat, once it has started") {
    FrameCapSchedule s;
    s.SetPeriod(kPeriod);
    s.Nudge(1'000'000);  // nothing to move yet
    CHECK(s.Next(kStart) == kStart);
    s.Nudge(-2'000'000);
    CHECK(s.Next(kStart + 1'000'000) == kStart + kPeriod - 2'000'000);
    s.Nudge(500'000);
    CHECK(s.Next(kStart + kPeriod) == kStart + 2 * kPeriod - 1'500'000);
}

using band3::pacing::PhaseLock;

TEST_CASE("the phase lock moves the beat toward half a period of slack") {
    constexpr int64_t p = 16'666'667;
    // published right before its paint: the beat moves earlier
    CHECK(PhaseLock::Correction(2'000'000, p) < 0);
    CHECK(PhaseLock::Correction(p / 2, p) == 0);
    // published long before: later, for less lag
    CHECK(PhaseLock::Correction(14'000'000, p) > 0);
    // at most a quarter period at a time
    CHECK(PhaseLock::Correction(0, p) >= -p / 4);
    CHECK(PhaseLock::Correction(100'000'000, p) <= p / 4);
}

TEST_CASE("the phase lock's slack follows the display's rate unless it's set") {
    using band3::pacing::PhaseLockSlackPercent;
    CHECK(PhaseLockSlackPercent(0, 120) == 65);
    CHECK(PhaseLockSlackPercent(0, 144) == 65);
    CHECK(PhaseLockSlackPercent(0, 100) == 65);
    CHECK(PhaseLockSlackPercent(0, 60) == 50);
    CHECK(PhaseLockSlackPercent(0, 90) == 50);
    CHECK(PhaseLockSlackPercent(40, 120) == 40);
    CHECK(PhaseLockSlackPercent(70, 60) == 70);
    CHECK(PhaseLockSlackPercent(5, 120) == 10);
}

TEST_CASE("the phase lock steers to a slack it's given") {
    constexpr int64_t p = 8'333'333;
    constexpr int64_t target = p * 30 / 100;
    CHECK(PhaseLock::Correction(target, p, target) == 0);
    // half a period is more than this target asks for: later, for less lag
    CHECK(PhaseLock::Correction(p / 2, p, target) > 0);
    CHECK(PhaseLock::Correction(1'000'000, p, target) < 0);
    // 0 is half the period, as before
    CHECK(PhaseLock::Correction(p / 2, p, 0) == 0);
}

TEST_CASE("the phase lock counts each frame's first paint once a window is full") {
    constexpr int64_t p = 16'666'667;
    PhaseLock lock;
    int64_t t = 1'000'000'000;
    int64_t correction = 0;
    for (uint64_t serial = 1; serial <= PhaseLock::kWindow; serial++) {
        lock.Published(t, t + 15'000'000);
        // published 2 ms before the paint
        correction = lock.Shown(serial, t, t + 17'000'000, p);
        if (serial < PhaseLock::kWindow) CHECK(correction == 0);
        // a repeat of the same frame doesn't count
        CHECK(lock.Shown(serial, t, t + 17'000'000 + p, p) == 0);
        t += p;
    }
    CHECK(correction == PhaseLock::Correction(2'000'000, p));
    // a frame it never saw published, a slack below 0 or a stall's don't count
    CHECK(lock.Shown(100, 5, 10, p) == 0);
    lock.Published(t, t + 20'000'000);
    CHECK(lock.Shown(101, t, t + 19'000'000, p) == 0);
    lock.Published(t + 1, t + 1);
    CHECK(lock.Shown(102, t + 1, t + 1 + 4 * p, p) == 0);
}

TEST_CASE("locked, frames are published about half a refresh before their paint") {
    // a display whose vblanks fall at an odd phase, a frame cap at its rate,
    // and frames published 15 ms after their Present give or take 2 ms: the
    // slack starts at about 1 ms, where the slower frames miss their paint
    constexpr int64_t p = 16'666'667;
    const int64_t phase = 3'300'000;
    FrameCapSchedule cap;
    cap.SetPeriod(p);
    PhaseLock lock;
    int64_t pending = 0, now = 0, last_slack = 0, last_paint = 0;
    int misses = 0;
    for (uint64_t frame = 1; frame <= 1200; frame++) {
        const int64_t take = std::clamp(pending, -band3::pacing::kNudgePerFrameNs,
                                        band3::pacing::kNudgePerFrameNs);
        pending -= take;
        cap.Nudge(take);
        const int64_t present = cap.Next(now);
        const int64_t jitter = int64_t((frame * 7919) % 5) * 1'000'000 - 2'000'000;
        const int64_t published = present + 15'000'000 + jitter;
        const int64_t paint = phase + ((published - phase + p - 1) / p) * p;
        lock.Published(present, published);
        pending += lock.Shown(frame, present, paint, p);
        last_slack = paint - published;
        // the frame's paint isn't the one after the frame before's
        if (frame > 1000 && last_paint && paint != last_paint + p) misses++;
        last_paint = paint;
        now = present + 2'000'000;
    }
    // jitter of 2 ms either way, settled around half the period
    CHECK(last_slack > p / 2 - 4'000'000);
    CHECK(last_slack < p / 2 + 4'000'000);
    CHECK(misses == 0);
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

using band3::pacing::PacePublishing;

TEST_CASE("frames are paced only below 1.5 times the display's refresh") {
    // the cap at the refresh: the hold keeps a fast frame from one paint with
    // the frame before
    CHECK(PacePublishing(FrameCapMode::kDisplay, 120, 120));
    CHECK(PacePublishing(FrameCapMode::kFixed, 144, 144));
    CHECK(PacePublishing(FrameCapMode::kFixed, 60, 120));
    CHECK(PacePublishing(FrameCapMode::kFixed, 170, 120));
    // two frames or more a refresh: each paint shows the newest anyway
    CHECK_FALSE(PacePublishing(FrameCapMode::kFixed, 180, 120));
    CHECK_FALSE(PacePublishing(FrameCapMode::kFixed, 240, 120));
    CHECK_FALSE(PacePublishing(FrameCapMode::kFixed, 240, 60));
    // the cap off: the game at its refresh_rate
    CHECK(PacePublishing(FrameCapMode::kOff, 60, 60));
    CHECK_FALSE(PacePublishing(FrameCapMode::kOff, 120, 60));
}

using band3::pacing::PhaseLockSteers;

TEST_CASE("the phase lock steers a fixed cap at the display's rate") {
    CHECK(PhaseLockSteers(true, FrameCapMode::kDisplay, 120, 120));
    CHECK(PhaseLockSteers(true, FrameCapMode::kFixed, 120, 119.88));
    CHECK(PhaseLockSteers(true, FrameCapMode::kFixed, 144, 144.5));
    // off, another rate, auto, no cap or a display not known
    CHECK_FALSE(PhaseLockSteers(false, FrameCapMode::kDisplay, 120, 120));
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kFixed, 100, 120));
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kFixed, 240, 120));
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kAuto, 114, 120));
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kOff, 120, 120));
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kFixed, 120, 0));
}

TEST_CASE("frames aren't paced while the phase lock steers at 100 Hz or more") {
    CHECK_FALSE(PacePublishing(FrameCapMode::kDisplay, 120, 120, true));
    CHECK_FALSE(PacePublishing(FrameCapMode::kDisplay, 100, 100, true));
    // below that the lock keeps the hold
    CHECK(PacePublishing(FrameCapMode::kDisplay, 60, 60, true));
    CHECK(PacePublishing(FrameCapMode::kFixed, 90, 90, true));
    // where it doesn't (100 on 120 Hz, or the lock off), they are
    CHECK(PacePublishing(FrameCapMode::kFixed, 100, 120,
                         PhaseLockSteers(true, FrameCapMode::kFixed, 100, 120)));
    CHECK(PacePublishing(FrameCapMode::kDisplay, 120, 120,
                         PhaseLockSteers(false, FrameCapMode::kDisplay, 120, 120)));
}

TEST_CASE("auto never paces, and a rate not known keeps pacing") {
    // a variable refresh display shows each frame as it comes
    CHECK_FALSE(PacePublishing(FrameCapMode::kAuto, 114, 120));
    CHECK_FALSE(PacePublishing(FrameCapMode::kAuto, 0, 0));
    CHECK(PacePublishing(FrameCapMode::kFixed, 240, 0));
    CHECK(PacePublishing(FrameCapMode::kOff, 0, 120));
}

TEST_CASE("VR's game rate: the headset's refresh exactly, a number, or frame_cap's") {
    using namespace band3::pacing;
    // auto at a 120 Hz headset: its own period, to the nanosecond
    const FrameCap a = VrGameCap("auto", 8333333);
    CHECK(a.mode == FrameCapMode::kVr);
    CHECK(a.period_ns == 8333333);
    CHECK(a.hz == doctest::Approx(120).epsilon(1e-6));
    CHECK(VrGameCap("auto", 11111111).hz == doctest::Approx(90).epsilon(1e-6));
    // the refresh not known yet, or past what a cap takes: none
    CHECK(VrGameCap("auto", 0).period_ns == 0);
    CHECK(VrGameCap("auto", 1000000).period_ns == 0);  // 1000 Hz
    // a number, kept to the cap's range, whatever the headset
    const FrameCap n = VrGameCap("90", 8333333);
    CHECK(n.mode == FrameCapMode::kVr);
    CHECK(n.period_ns == 11111111);
    CHECK(VrGameCap("1000", 8333333).hz == doctest::Approx(kMaxCapHz));
    // off and the rest: frame_cap's as without VR
    CHECK(VrGameCap("off", 8333333).period_ns == 0);
    CHECK(VrGameCap("display", 8333333).period_ns == 0);
    CHECK(VrGameCap("fast", 8333333).period_ns == 0);
    CHECK(ValidVrGameRate("auto"));
    CHECK(ValidVrGameRate("off"));
    CHECK(ValidVrGameRate("72"));
    CHECK_FALSE(ValidVrGameRate("display"));
    CHECK_FALSE(ValidVrGameRate("fast"));
    // the phase lock never steers it: it's paced by the headset, not the window
    CHECK_FALSE(PhaseLockSteers(true, FrameCapMode::kVr, 120, 120));
    CHECK(std::string(FrameCapModeName(FrameCapMode::kVr)) == "vr");
}