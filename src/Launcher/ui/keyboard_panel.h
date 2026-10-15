#pragma once
#include <string_view>
#include <imgui.h>
#include "onscreen_keyboard.h"
#include "screen_kit.h"
#include "skin.h"

// The on-screen keyboard as the screens draw it in their detail panel: a
// heading, the entry with its caret, the keys, and a hint under them; and a
// plugged-in keyboard typing into it.

namespace band3::launcher::ui {

struct KeyboardPanel {
    std::string_view heading;
    std::string_view hint;
    // the entry shows dots
    bool secret = false;
    // where it goes, in layout pixels
    float x = 0, y = 0, width = 0;
};

// draws it; a key the mouse clicked is pressed (`clicks`: the mouse may
// press keys this frame). Returns what pressing it did.
OnScreenKeyboard::Result DrawKeyboardPanel(ImDrawList* dl, const Page& page, OnScreenKeyboard& keyboard,
                                           const KeyboardPanel& look, const Skin& skin, bool clicks);

// a plugged-in keyboard's typing: its characters and Backspace edit the text
// (`edited` set when they did), Enter is Done, Escape gives up. Asks the
// system for typed text while it's called.
OnScreenKeyboard::Result TypeIntoKeyboard(OnScreenKeyboard& keyboard, ImGuiIO& io, bool& edited);

}
