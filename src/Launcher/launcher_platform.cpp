#include "launcher_platform.h"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace band3::launcher {

bool ShiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

double DisplayRefreshRate(void* native_window) {
#ifdef _WIN32
    HMONITOR monitor =
        MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    // 0 and 1 stand for the hardware's default rate
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
#else
    (void)native_window;
    // band3's SDL copy only has video while the native view runs; the SDK's
    // copy owns the window, so this is the primary display's rate at best
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    return mode && mode->refresh_rate > 0 ? mode->refresh_rate : 0;
#endif
}

}
