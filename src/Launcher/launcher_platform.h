#pragma once

#include <cstdint>

// What band3 asks the system directly, outside the SDK: the launcher before
// the game (and its input system) runs, and the frame cap (frame_pacing.h)
// while it does.

namespace band3::launcher {

// whether Shift is held right now; Windows only (false elsewhere)
bool ShiftHeld();

// a display's refresh rate as the fraction it really runs at: 120000/1001 for
// a "120 Hz" mode at 119.88. Both 0 when it can't be told.
struct RefreshRate {
    uint32_t num = 0;
    uint32_t den = 0;
};

// the refresh rate of the display a native window (HWND) is on. On Windows
// the display path's own fraction, else the mode's whole Hz; elsewhere the
// primary display's, and only while band3's SDL has video (the native view)
RefreshRate DisplayRefresh(void* native_window);

// the same in Hz; 0 when it can't be told
double DisplayRefreshRate(void* native_window);

// what can be seen of a native window (HWND)
struct WindowShown {
    bool minimized = false;
    bool visible = true;
    uint32_t client_w = 0, client_h = 0;  // its client area, in pixels
};

// Windows only, as the system tells it now: false elsewhere, or for no
// window, leaving `out` as it was
bool NativeWindowShown(void* native_window, WindowShown& out);

}

namespace band3::pacing {

// Waits until `deadline_ns` (std::chrono::steady_clock's nanoseconds): a
// high-resolution timer to just short of it, then a spin on the clock, as no
// OS sleep wakes to within a millisecond reliably. Never raises the system
// timer's resolution (timeBeginPeriod). Returns how long it spun.
int64_t WaitUntil(int64_t deadline_ns);

// Sleeps for about `ns` on the same timer, without the spin: up to a few
// hundred microseconds longer on Windows 10 1803 and later, on the system
// timer's tick before, tens of microseconds elsewhere. For a short poll
// (native_view.cpp's wait for a GPU fence) that shouldn't cost a core.
void SleepFor(int64_t ns);

}
