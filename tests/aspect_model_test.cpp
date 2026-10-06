// Checks src/Hooks/aspect_model.h: the shape native_fill_window builds the
// game's cameras for, the YRatio it gives them, and the vertical field of
// view a window taller than 16:9 widens them by.

#include <doctest/doctest.h>
#include <cmath>
#include "src/Hooks/aspect_model.h"

using namespace band3::aspect;

namespace {

// RndCam::UpdateLocal's x and y scales for a field of view and YRatio, with
// its fov scale of a half (aspect.cpp reads it from the game)
double XScale(float fov, double yratio) { return yratio / std::tan(fov * 0.5); }
double YScale(float fov) { return 1.0 / std::tan(fov * 0.5); }

}  // namespace

TEST_CASE("the game's own 16:9 when not filling, or the window is 16:9") {
    CHECK(ShapeFor(2560, 1080, false) == 0);
    CHECK(ShapeFor(1920, 1080, true) == 0);
    CHECK(ShapeFor(1280, 720, true) == 0);
    // within half a percent
    CHECK(ShapeFor(1366, 768, true) == 0);
    // minimized
    CHECK(ShapeFor(0, 0, true) == 0);
    CHECK(YRatio(0) == doctest::Approx(9.0 / 16.0));
    CHECK(TallScale(0) == 1.0);
}

TEST_CASE("a wider window: its own YRatio and no widening") {
    const uint64_t shape = ShapeFor(3440, 1440, true);
    REQUIRE(shape != 0);
    CHECK(ShapeWidth(shape) == 3440);
    CHECK(ShapeHeight(shape) == 1440);
    CHECK(YRatio(shape) == doctest::Approx(1440.0 / 3440.0));
    CHECK(TallScale(shape) == 1.0);
    // nothing stretched: the x scale times the window's width over height is
    // 16:9's, so it keeps 16:9's height and shows 2.39 / (16 / 9) as much
    // across
    const float fov = 0.6f;
    CHECK(XScale(fov, YRatio(shape)) * (3440.0 / 1440.0) ==
          doctest::Approx(XScale(fov, 9.0 / 16.0) * (16.0 / 9.0)));
}

TEST_CASE("a taller window keeps 16:9's sides and shows more above and below") {
    const uint64_t shape = ShapeFor(1920, 1200, true);
    REQUIRE(shape != 0);
    CHECK(TallScale(shape) == doctest::Approx(0.9));
    for (float fov : {0.3f, 0.6f, 1.164f, 1.438f}) {
        const float widened = WidenedFov(fov, 0.5f, TallScale(shape));
        CHECK(widened > fov);
        // what 16:9 shows across
        CHECK(XScale(widened, YRatio(shape)) == doctest::Approx(XScale(fov, 9.0 / 16.0)));
        // and 1.6 / (16 / 9) as much up and down
        CHECK(YScale(widened) == doctest::Approx(YScale(fov) * 0.9));
    }
    // a 4:3 window too
    const uint64_t square = ShapeFor(1024, 768, true);
    const float widened = WidenedFov(0.6f, 0.5f, TallScale(square));
    CHECK(XScale(widened, YRatio(square)) == doctest::Approx(XScale(0.6f, 9.0 / 16.0)));
}

TEST_CASE("the overlay's edge is where the game's 16:9 ends in the picture") {
    float edge[2];
    OverlayEdge(0, edge);
    CHECK(edge[0] == 1.0f);
    CHECK(edge[1] == 1.0f);
    // 21:9's 16:9 is the middle three quarters across
    OverlayEdge(ShapeFor(2560, 1080, true), edge);
    CHECK(edge[0] == doctest::Approx((16.0 / 9.0) / (2560.0 / 1080.0)));
    CHECK(edge[1] == 1.0f);
    // 16:10's the middle nine tenths down
    OverlayEdge(ShapeFor(1920, 1200, true), edge);
    CHECK(edge[0] == 1.0f);
    CHECK(edge[1] == doctest::Approx(0.9));
}
