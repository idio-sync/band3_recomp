#pragma once
#include <chrono>
#include <cstdint>

// Opens band3's menus from a controller, for when there's no keyboard (a Steam
// Deck in Game Mode): hold both stick clicks for a second for the settings menu
// (bind_settings, F4), or both stick clicks and the left bumper for the
// Instrument Lab (bind_instrument_lab, F6). Guitars and drums have no stick
// clicks, so playing never triggers it. A chord fires once, then waits for its
// buttons to be let go.

namespace band3::input {

enum class MenuShortcutAction { kNone, kSettings, kInstrumentLab };

class MenuShortcut {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::duration kHoldTime = std::chrono::seconds(1);

    // buttons: the XINPUT_GAMEPAD button bits held, on any pad
    MenuShortcutAction Update(uint16_t buttons, Clock::time_point now);

private:
    MenuShortcutAction held_ = MenuShortcutAction::kNone;
    Clock::time_point held_since_{};
    // the chord fired and its buttons haven't all been let go since
    bool fired_ = false;
};

}
