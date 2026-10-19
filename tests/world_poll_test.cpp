// Checks src/Hooks/world_poll.h: a per-poll step keeps the game's own size at
// the venue's rate and shrinks when the world is polled more often
// (background_fps), so the camera shake and hair keep their speed.

#include <doctest/doctest.h>
#include "src/Hooks/world_poll.h"

using band3::world_poll::StepScale;

TEST_CASE("the venue's own rate keeps the game's step") {
    CHECK(StepScale(2, 60, 30) == doctest::Approx(1));   // 30 fps venue at 60 Hz
    CHECK(StepScale(4, 120, 30) == doctest::Approx(1));  // band3 keeps it at 120 Hz
    CHECK(StepScale(1, 60, 60) == doctest::Approx(1));   // no emulation
}

TEST_CASE("polled every frame, a 30 fps venue's step halves") {
    CHECK(StepScale(1, 60, 30) == doctest::Approx(0.5));
    CHECK(StepScale(1, 120, 30) == doctest::Approx(0.25));
}

TEST_CASE("outside a poll, or with nothing known, the step is the game's") {
    CHECK(StepScale(0, 60, 30) == 1);
    CHECK(StepScale(2, 0, 30) == 1);
    CHECK(StepScale(2, 60, 0) == 1);
}
