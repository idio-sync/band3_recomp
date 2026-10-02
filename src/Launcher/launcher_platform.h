#pragma once

// What the launcher asks the system directly, before the game (and its input
// system) runs.

namespace band3::launcher {

// whether Shift is held right now; Windows only (false elsewhere)
bool ShiftHeld();

// the refresh rate of the display a native window (HWND) is on, in Hz; 0 when
// it can't be told
double DisplayRefreshRate(void* native_window);

}
