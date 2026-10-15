#pragma once
#include <vector>
#include "navigation.h"

// The keyboard's part of the launcher's navigation (navigation.h): ImGui's
// key presses this frame, with its key repeat, as actions. Arrows move, Enter
// and Space select, Esc and Backspace go back, / is the secondary action, R
// resets, Tab and Shift+Tab change category, F5 plays.

namespace band3::launcher::ui {

std::vector<Action> KeyboardActions();

}
