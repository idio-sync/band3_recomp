#include "launcher_platform.h"
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>

#include <chrono>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <ctime>
#endif

// Windows 10 1803's; older SDKs don't name it
#ifdef _WIN32
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
#endif

namespace band3::launcher {

bool ShiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

#ifdef _WIN32
namespace {

// the refresh rate of the display path whose source is the GDI device
// `device` (\\.\DISPLAY1): the fraction the display really runs at, which
// DEVMODE rounds to whole Hz
RefreshRate PathRefresh(const wchar_t* device) {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result = ERROR_INSUFFICIENT_BUFFER;
    // the paths can change between asking how many and reading them
    for (int tries = 0; tries < 3 && result == ERROR_INSUFFICIENT_BUFFER; tries++) {
        UINT32 num_paths = 0, num_modes = 0;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &num_paths, &num_modes) !=
            ERROR_SUCCESS)
            return {};
        paths.resize(num_paths);
        modes.resize(num_modes);
        result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &num_paths, paths.data(), &num_modes,
                                    modes.data(), nullptr);
        paths.resize(num_paths);
    }
    if (result != ERROR_SUCCESS) return {};
    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            wcscmp(source.viewGdiDeviceName, device) != 0)
            continue;
        const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
        if (rate.Numerator > 0 && rate.Denominator > 0) return {rate.Numerator, rate.Denominator};
    }
    return {};
}

}
#endif

RefreshRate DisplayRefresh(void* native_window) {
#ifdef _WIN32
    HMONITOR monitor =
        MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return {};
    if (const RefreshRate exact = PathRefresh(info.szDevice); exact.num) return exact;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return {};
    // 0 and 1 stand for the hardware's default rate
    if (mode.dmDisplayFrequency > 1) return {mode.dmDisplayFrequency, 1};
    return {};
#else
    (void)native_window;
    // band3's SDL copy only has video while the native view runs; the SDK's
    // copy owns the window, so this is the primary display's rate at best
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return {};
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    if (!mode) return {};
    if (mode->refresh_rate_numerator > 0 && mode->refresh_rate_denominator > 0)
        return {static_cast<uint32_t>(mode->refresh_rate_numerator),
                static_cast<uint32_t>(mode->refresh_rate_denominator)};
    // without the fraction, the float's rate to a thousandth of a Hz
    if (mode->refresh_rate > 0) return {static_cast<uint32_t>(mode->refresh_rate * 1000 + 0.5), 1000};
    return {};
#endif
}

double DisplayRefreshRate(void* native_window) {
    const RefreshRate rate = DisplayRefresh(native_window);
    return rate.num && rate.den ? double(rate.num) / rate.den : 0;
}

}

namespace band3::pacing {

namespace {

int64_t Now() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void Pause() {
#if defined(_WIN32)
    YieldProcessor();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

#ifdef _WIN32
// The game thread's timer. A high-resolution one wakes within a few hundred
// microseconds of when it's set for without timeBeginPeriod; where Windows has
// none (before 10 1803) a plain one wakes on the system timer's tick, so it's
// set further short and the spin covers the rest.
struct Timer {
    HANDLE handle = nullptr;
    int64_t margin_ns = 0;

    Timer() {
        handle = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_ALL_ACCESS);
        margin_ns = 500'000;
        if (!handle) {
            handle = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
            margin_ns = 2'000'000;
        }
    }
    ~Timer() {
        if (handle) CloseHandle(handle);
    }
};
#endif

}

int64_t WaitUntil(int64_t deadline_ns) {
#ifdef _WIN32
    thread_local Timer timer;
    const int64_t sleep_ns = deadline_ns - timer.margin_ns - Now();
    if (timer.handle && sleep_ns > 0) {
        LARGE_INTEGER due;
        due.QuadPart = -(sleep_ns / 100);  // relative, in 100 ns
        if (SetWaitableTimer(timer.handle, &due, 0, nullptr, nullptr, FALSE))
            WaitForSingleObject(timer.handle, INFINITE);
    }
#else
    // clock_nanosleep wakes within tens of microseconds on Linux; steady_clock
    // is CLOCK_MONOTONIC there, so the deadline is the clock's own time
    const int64_t wake_ns = deadline_ns - 200'000;
    if (wake_ns > Now()) {
        timespec ts{static_cast<time_t>(wake_ns / 1'000'000'000),
                    static_cast<long>(wake_ns % 1'000'000'000)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {
        }
    }
#endif
    const int64_t spin_from = Now();
    int64_t now = spin_from;
    while (now < deadline_ns) {
        Pause();
        now = Now();
    }
    return now - spin_from;
}

}
