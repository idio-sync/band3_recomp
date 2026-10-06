#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include "instruments.h"

// The mouse in the game's menus (mouse_menus): left click presses A, right
// click B (back), and each notch of the wheel presses the d-pad up or down. The
// presses join what the game reads from the lowest connected player
// (src/Input/mouse_menus_driver.cpp, src/Hooks/input_lock.cpp), and stop while
// a song is on, where A and B are frets.
//
// This is the timing, kept apart from the SDK so it can be unit tested. Every
// press lasts at least kPulse, and presses that come quicker than that wait
// their turn, with a kPulse gap before each: the game's joypad thread reads
// every 4 ms, but a menu acts on what it sees once a frame, so a quick
// double click or a fast wheel would otherwise run together into one press.

namespace band3::input {

class MouseMenus {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::duration kPulse = std::chrono::milliseconds(40);
    // presses waiting on one button at most; more are dropped, so a fast
    // spin of the wheel doesn't keep scrolling long after it stops
    static constexpr int kMaxQueued = 4;

    enum class Button { kLeft, kRight };

    void Press(Button button);
    void Release(Button button);
    // `amount` in the window's units, `per_notch` of them a notch, up
    // positive. Touchpads scroll in parts of a notch, which add up.
    void Scroll(int32_t amount, int32_t per_notch);
    // forgets every press, held or waiting
    void Clear();

    // the XINPUT_GAMEPAD button bits pressed at `now`. Each call moves each
    // button on at most one step (a press starting or ending), so the game
    // sees every press and every gap between them.
    uint16_t Buttons(Clock::time_point now);

private:
    struct Key {
        uint16_t bit = 0;
        // the mouse button is down (clicks only)
        bool held = false;
        // presses waiting for the one before to end
        int queued = 0;
        bool on = false;
        // a press lasts until at least here, and the gap after it until here
        Clock::time_point on_until{};
        Clock::time_point off_until{};

        void Queue();
        bool Step(Clock::time_point now);
    };

    Key& KeyFor(Button button) { return button == Button::kLeft ? keys_[0] : keys_[1]; }

    // A, B, d-pad up, d-pad down
    std::array<Key, 4> keys_ = {Key{.bit = xbox::kButtonA}, Key{.bit = xbox::kButtonB},
                                Key{.bit = xbox::kDpadUp}, Key{.bit = xbox::kDpadDown}};
    // scrolled since the last whole notch
    int32_t scrolled_ = 0;
};

}
