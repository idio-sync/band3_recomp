// Checks src/Hooks/frame_pacing.h: the world's period under even/odd
// rendering matches RB3's own at 60 Hz, keeps the venue's rate at higher
// refresh rates, and follows background_fps when it's set.

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
