// Checks which backend the SDK builds for an input_backend value
// (src/Input/input_backend.h).

#include <doctest/doctest.h>
#include <ostream>
#include "src/Input/input_backend.h"

using band3::input::BackendFor;

TEST_CASE("only an exact xinput on Windows is XInput; anything else is SDL") {
    CHECK(BackendFor("xinput", true) == "xinput");
    CHECK(BackendFor("sdl", true) == "sdl");
    // the SDK compares case sensitively, and builds SDL for anything it doesn't know
    CHECK(BackendFor("XInput", true) == "sdl");
    CHECK(BackendFor("SDL", true) == "sdl");
    CHECK(BackendFor("sdl2", true) == "sdl");
    CHECK(BackendFor("", true) == "sdl");
    // always SDL off Windows
    CHECK(BackendFor("xinput", false) == "sdl");
    CHECK(BackendFor("sdl", false) == "sdl");
}
