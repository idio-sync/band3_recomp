#pragma once
#include <cstdint>
#include <memory>
#include <optional>

namespace rex::input {
class InputDriver;
struct X_INPUT_STATE;
}

namespace band3::input {

// The mouse in the game's menus (mouse_menus.h), for the input system: a
// driver with no device of its own, so it never takes a player or changes the
// type the game reads for one (the SDK hands a player's type from the device
// last used, and band3's pro instruments read it every poll). It watches the
// window's mouse buttons and wheel, and takes a click only when it lands on
// the game: not over one of band3's or the SDK's windows or under a dialog
// they show, not while a song is on or a band3 dialog has the controller
// (GameInputBlocked), and only with the window in focus.
std::unique_ptr<rex::input::InputDriver> CreateMouseMenusDriver();

// From the game's XInputGetState (src/Hooks/input_lock.cpp), after each read:
// `state` is what the game read for `user`, or null when the player has no
// controller. The lowest connected player gets the mouse's presses, ORed into
// its buttons, with the packet number moved on when they change.
void AddMouseMenuPresses(uint32_t user, rex::input::X_INPUT_STATE* state);

// Where the pointer is over the game, for hovering a list's rows
// (src/Hooks/mouse_hover.cpp): in the window's pixels, with the window's size
// then. Only moves the mouse may make count (as clicks: the window in focus,
// not over band3's or the SDK's windows, no song on), numbered by `moves`.
struct HoverPointer {
    uint64_t moves = 0;
    int32_t x = 0, y = 0;
    uint32_t width = 0, height = 0;
};

// Game thread: the pointer, if it has moved since move `seen`
std::optional<HoverPointer> TakeHoverPointer(uint64_t seen);
// Any thread: the pointer as last moved
HoverPointer LastHoverPointer();
// Game thread: the hover has acted on move `moves`; a click made with it can
// now go to the game
void HoverApplied(uint64_t moves);

// the player (0-3) the mouse's presses go to: the lowest connected
uint32_t MouseMenuPlayer();

// UI thread: the test harness's `mouse` hands the window events while it
// doesn't have focus; they count as a focused window's while this is on
void MouseAsFocused(bool on);

}
