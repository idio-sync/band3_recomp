// Checks src/Render/gpu_view.h's SubmitAtResolve, where GpuRenderer's frame
// goes to the GPU in parts beyond the texture passes' mips
// (native_view_submit_points): at the resolve with 1, never for a world pass
// before the frame or the world drawn ahead, and never with nothing new to
// give the GPU.

#include <doctest/doctest.h>
#include <cstdint>
#include "src/Render/gpu_view.h"

using namespace band3::render;

TEST_CASE("the resolve submits the world with submit points 1, not with 0") {
    CHECK_FALSE(SubmitAtResolve(0, false, 500));
    CHECK(SubmitAtResolve(1, false, 500));
    CHECK(SubmitAtResolve(2, false, 1));
}

TEST_CASE("a world pass before the frame and the world drawn ahead never submit at the resolve") {
    // they end at the resolve and are submitted there anyway
    CHECK_FALSE(SubmitAtResolve(1, true, 500));
    CHECK_FALSE(SubmitAtResolve(2, true, 500));
}

TEST_CASE("nothing drawn since the last submission: no submission for nothing") {
    // a frame showing the kept post buffer whose last texture pass made mips
    // (submitted there) has no world of its own
    CHECK_FALSE(SubmitAtResolve(1, false, 0));
}
