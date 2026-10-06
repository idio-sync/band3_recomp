#pragma once
#include <memory>
#include <optional>
#include <rex/input/input_driver.h>
#include "src/Input/keyboard_search.h"

// Typing to search the song list (keyboard_search.h): a listener on the game's
// window that takes the keys that type there before the keyboard's controller
// binds see them, and queues what they send for the game thread
// (Hooks/keyboard_search.cpp). It's a driver with no devices, as the mouse
// menus' is, for the window it's handed.

namespace band3::input {

std::unique_ptr<rex::input::InputDriver> CreateKeyboardSearchDriver();

// Game thread, each frame: what the keyboard does now, and the next key to
// send the game, if any. The window reads the mode as each key goes down.
void SetKeyboardSearchMode(keyboard_search::Mode mode);
std::optional<keyboard_search::Key> TakeKeyboardSearchKey();

// UI thread: the test harness's `type` hands the window keys while it doesn't
// have focus; they count as a focused window's while this is on
void TypeAsFocused(bool on);

}
