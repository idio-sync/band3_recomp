#pragma once
#include <cstdint>
#include <optional>
#include <string>

// Typing on the song list to search it, as RB3Enhanced brings back on the Xbox
// 360 (source/xbox_keyboard.c): Harmonix took the game's keyboard code out of
// the 360 version, and RB3E polls the keystrokes each frame and runs
// {ui key $key $shift $ctrl $alt}, which Rock Band 3 Deluxe's scripts handle
// (dx_keyboard_handler, _ark/dx/ui/dx_keyboard.dta): the song list opens its
// search with the key typed, and a focused input field takes every key.
//
// Here the keys come from the window rather than the guest's
// XInputGetKeystroke, since the keyboard is also a controller: the keys that
// type are kept from the controller's binds while they do. This decides which
// keys those are and what each sends; keyboard_search_driver.cpp listens to the
// window and keyboard_search.cpp (in Hooks) runs the messages on the game
// thread.

namespace band3::input::keyboard_search {

// what the keyboard does in the game at the moment
enum class Mode {
    // the controller's binds only (no Deluxe search, in a song, a dialog up)
    kOff,
    // the song list or Deluxe's search screen, nothing focused: the keys that
    // type a character open the search with it, the rest stay the binds
    kSongList,
    // one of Deluxe's input fields is focused (the search, the console): it
    // takes every key but Escape and the function keys
    kField,
};

// a key for the game, in Harmonix's key codes (the game's KB_* macros): the
// character for one that types, Dance Central 3's TranslateVK for the others
struct Key {
    int code = 0;
    bool shift = false;
    bool ctrl = false;
    bool alt = false;

    bool operator==(const Key&) const = default;
};

struct DownAction {
    // keep the key from the controller's binds (and anything else listening)
    bool consume = false;
    // what to send now
    std::optional<Key> send;
    // a key that types: its character comes after, to OnChar
    bool types = false;
};

// A key going down (or repeating). vk: a Windows virtual key code.
DownAction OnKeyDown(Mode mode, uint16_t vk, bool shift, bool ctrl, bool alt);

// The character a key typed (the window's text input), after its OnKeyDown:
// what to send.
std::optional<Key> OnChar(Mode mode, uint32_t ch, bool shift, bool ctrl, bool alt);

// The character a key types on a US keyboard, for a key whose own character
// won't come: the window only sends characters while it's taking text, which
// it starts at the first key that types (keyboard_search_driver.cpp).
std::optional<uint32_t> UsCharacter(uint16_t vk, bool shift);

// {ui key $key $shift $ctrl $alt}
std::string Script(const Key& key);

}
