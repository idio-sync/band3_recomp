#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <optional>

// Opens band3's menus from a controller, for when there's no keyboard (a Steam
// Deck in Game Mode): hold both stick clicks for a second for the settings menu
// (bind_settings, F4), or both stick clicks and the left bumper for the
// Instrument Lab (bind_instrument_lab, F6). Only controllers count: a guitar's
// solo frets and a drum kit's pads and second kick send stick clicks too
// (instruments.cpp), so playing an instrument never triggers it. A chord fires
// once, then waits for its buttons to be let go.

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

// What the game last read from each player's controller, for the shortcut. The
// game's own reads are the ones to watch: band3's SDL has no gamepads open (the
// SDK's runtime has its own SDL), and a read of a player through the SDK's
// input system would change which of the player's devices the game reads.
// The game's XInputGetState and XInputGetCapabilities (src/Hooks/input_lock.cpp)
// report here from its joypad thread; the shortcut reads on the UI thread.
class ChordPads {
public:
    using Clock = MenuShortcut::Clock;
    // a player not read for this long counts as holding nothing, so a chord
    // held as the game stops reading doesn't stay held
    static constexpr Clock::duration kStale = std::chrono::milliseconds(500);

    // the device type (XINPUT_CAPABILITIES' SubType) the game read for a player
    void OnCapabilities(uint32_t player, uint8_t subtype);
    // the buttons the game read for a player, or nullopt for no controller
    void OnState(uint32_t player, std::optional<uint16_t> buttons, Clock::time_point now);
    // the buttons held, as XINPUT_GAMEPAD bits, on every player's controller
    // that isn't an instrument, read within kStale of `now`
    uint16_t Held(Clock::time_point now) const;

private:
    struct Player {
        // no type read yet counts as an instrument
        std::optional<uint8_t> subtype;
        uint16_t buttons = 0;
        Clock::time_point read{};
    };
    mutable std::mutex mutex_;
    std::array<Player, 4> players_{};
};

// the game's (src/Hooks/input_lock.cpp)
ChordPads& GameChordPads();

}
