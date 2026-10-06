// Checks the mouse in the game's menus (src/Input/mouse_menus.cpp): clicks and
// wheel notches as presses long enough, and far enough apart, for the game to
// see each one.

#include <doctest/doctest.h>
#include <chrono>
#include <cstdint>
#include "src/Input/instruments.h"
#include "src/Input/mouse_menus.h"

using namespace band3::input;
using namespace std::chrono_literals;

namespace {

using Button = MouseMenus::Button;
const MouseMenus::Clock::time_point t0{};
constexpr auto kPulse = MouseMenus::kPulse;
// the game's joypad thread reads this often
constexpr auto kPoll = 4ms;

}

TEST_CASE("a left click presses A and a right click B, for as long as they're held") {
    MouseMenus mouse;
    mouse.Press(Button::kLeft);
    CHECK(mouse.Buttons(t0) == xbox::kButtonA);
    CHECK(mouse.Buttons(t0 + 1s) == xbox::kButtonA);
    mouse.Release(Button::kLeft);
    CHECK(mouse.Buttons(t0 + 1s + kPoll) == 0);

    mouse.Press(Button::kRight);
    CHECK(mouse.Buttons(t0 + 2s) == xbox::kButtonB);
    CHECK(mouse.Buttons(t0 + 3s) == xbox::kButtonB);
    mouse.Release(Button::kRight);
    CHECK(mouse.Buttons(t0 + 3s + kPoll) == 0);
}

TEST_CASE("a click quicker than a pulse still lasts a pulse") {
    MouseMenus mouse;
    mouse.Press(Button::kLeft);
    mouse.Release(Button::kLeft);
    CHECK(mouse.Buttons(t0) == xbox::kButtonA);
    CHECK(mouse.Buttons(t0 + kPulse - kPoll) == xbox::kButtonA);
    CHECK(mouse.Buttons(t0 + kPulse) == 0);
}

TEST_CASE("a double click is two presses, with a gap between") {
    MouseMenus mouse;
    mouse.Press(Button::kLeft);
    CHECK(mouse.Buttons(t0) == xbox::kButtonA);
    mouse.Release(Button::kLeft);
    mouse.Press(Button::kLeft);
    // the second click is still held, but the first press ends on time
    CHECK(mouse.Buttons(t0 + 10ms) == xbox::kButtonA);
    CHECK(mouse.Buttons(t0 + kPulse) == 0);
    CHECK(mouse.Buttons(t0 + kPulse + kPulse - kPoll) == 0);
    CHECK(mouse.Buttons(t0 + kPulse + kPulse) == xbox::kButtonA);
    // and the second lasts while held
    CHECK(mouse.Buttons(t0 + 1s) == xbox::kButtonA);
    mouse.Release(Button::kLeft);
    CHECK(mouse.Buttons(t0 + 1s + kPoll) == 0);
}

TEST_CASE("a step at a time: a late read still sees the gap") {
    MouseMenus mouse;
    mouse.Press(Button::kRight);
    mouse.Release(Button::kRight);
    mouse.Press(Button::kRight);
    mouse.Release(Button::kRight);
    CHECK(mouse.Buttons(t0) == xbox::kButtonB);
    // long after both clicks: the first press ends, then the second starts
    CHECK(mouse.Buttons(t0 + 1s) == 0);
    CHECK(mouse.Buttons(t0 + 1s + kPulse) == xbox::kButtonB);
    CHECK(mouse.Buttons(t0 + 1s + 2 * kPulse) == 0);
    CHECK(mouse.Buttons(t0 + 5s) == 0);
}

TEST_CASE("each wheel notch presses the d-pad once") {
    MouseMenus mouse;
    constexpr int32_t kNotch = 120;
    mouse.Scroll(2 * kNotch, kNotch);
    auto t = t0;
    CHECK(mouse.Buttons(t) == xbox::kDpadUp);
    t += kPulse;
    CHECK(mouse.Buttons(t) == 0);
    t += kPulse;
    CHECK(mouse.Buttons(t) == xbox::kDpadUp);
    t += kPulse;
    CHECK(mouse.Buttons(t) == 0);
    t += 1s;
    CHECK(mouse.Buttons(t) == 0);

    mouse.Scroll(-kNotch, kNotch);
    CHECK(mouse.Buttons(t) == xbox::kDpadDown);
    t += kPulse;
    CHECK(mouse.Buttons(t) == 0);
    t += 1s;
    CHECK(mouse.Buttons(t) == 0);
}

TEST_CASE("parts of a notch add up, and turning back starts afresh") {
    MouseMenus mouse;
    constexpr int32_t kNotch = 120;
    mouse.Scroll(50, kNotch);
    mouse.Scroll(50, kNotch);
    CHECK(mouse.Buttons(t0) == 0);
    mouse.Scroll(20, kNotch);
    CHECK(mouse.Buttons(t0) == xbox::kDpadUp);
    CHECK(mouse.Buttons(t0 + kPulse) == 0);

    mouse.Scroll(100, kNotch);
    mouse.Scroll(-100, kNotch);
    CHECK(mouse.Buttons(t0 + 1s) == 0);
    mouse.Scroll(-20, kNotch);
    CHECK(mouse.Buttons(t0 + 1s) == xbox::kDpadDown);
}

TEST_CASE("a fast spin queues only a few notches, and turning back drops them") {
    MouseMenus mouse;
    constexpr int32_t kNotch = 120;
    mouse.Scroll(20 * kNotch, kNotch);
    int presses = 0;
    bool was_on = false;
    for (auto t = t0; t < t0 + 5s; t += kPoll) {
        const bool on = mouse.Buttons(t) == xbox::kDpadUp;
        if (on && !was_on) presses++;
        was_on = on;
    }
    CHECK(presses == MouseMenus::kMaxQueued);

    mouse.Scroll(3 * kNotch, kNotch);
    CHECK(mouse.Buttons(t0 + 10s) == xbox::kDpadUp);
    mouse.Scroll(-kNotch, kNotch);
    presses = 0;
    was_on = true;
    uint16_t seen = 0;
    for (auto t = t0 + 10s + kPoll; t < t0 + 15s; t += kPoll) {
        const uint16_t buttons = mouse.Buttons(t);
        seen |= buttons;
        const bool on = buttons == xbox::kDpadDown;
        if (on && !was_on) presses++;
        was_on = buttons != 0;
    }
    // the up press already on finishes; the two waiting are dropped
    CHECK(presses == 1);
    CHECK(seen == (xbox::kDpadUp | xbox::kDpadDown));
}

TEST_CASE("Clear forgets every press") {
    MouseMenus mouse;
    mouse.Press(Button::kLeft);
    mouse.Press(Button::kRight);
    mouse.Scroll(240, 120);
    CHECK(mouse.Buttons(t0) == (xbox::kButtonA | xbox::kButtonB | xbox::kDpadUp));
    mouse.Clear();
    CHECK(mouse.Buttons(t0 + kPoll) == 0);
    CHECK(mouse.Buttons(t0 + 1s) == 0);
    // a release after the clear changes nothing
    mouse.Release(Button::kLeft);
    CHECK(mouse.Buttons(t0 + 2s) == 0);
}
