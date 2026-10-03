#include "device_lists.h"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <memory>
#include <rex/logging.h>
#include "src/ThirdParty/rtmidi/RtMidi.h"

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

std::vector<std::string> RecordingDeviceNames() {
    // said once, not at every listing, until SDL's audio starts again
    static bool warned = false;
    std::vector<std::string> names;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        if (!warned) REXLOG_WARN("Launcher: can't list microphones ({})", SDL_GetError());
        warned = true;
        return names;
    }
    warned = false;
    int count = 0;
    if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            names.push_back(name ? name : "");
        }
        SDL_free(ids);
    }
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    return names;
}

std::vector<MidiPort> MidiInputPorts() {
    std::vector<MidiPort> ports;
    try {
        RtMidiIn probe;
        // RtMidi prints errors to stderr unless something takes them
        probe.setErrorCallback([](RtMidiError::Type, const std::string&, void*) {});
        const unsigned count = probe.getPortCount();
        for (unsigned i = 0; i < count; i++) {
            MidiPort port;
            port.port = probe.getPortName(i);
#ifdef _WIN32
            port.name = StripMidiPortIndex(port.port, i);
#else
            port.name = port.port;
#endif
            ports.push_back(std::move(port));
        }
    } catch (const RtMidiError& e) {
        REXLOG_WARN("Launcher: can't list MIDI ports ({})", e.getMessage());
    }
    return ports;
}

#ifdef _WIN32

namespace {

std::string Utf8(const wchar_t* text) {
    const int size = WideCharToMultiByte(CP_UTF8, 0, text, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text, -1, out.data(), size, nullptr, nullptr);
    return out;
}

// the monitor's own name, as SDL names it (WIN_GetDisplayNameVista): the
// display configuration's friendly name for the GDI device, or else the
// adapter's description
std::string MonitorName(const wchar_t* gdi_device) {
    UINT32 path_count = 0, mode_count = 0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG rc;
    do {
        rc = GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &path_count, &mode_count);
        if (rc != ERROR_SUCCESS) break;
        paths.resize(path_count);
        modes.resize(mode_count);
        rc = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &path_count, paths.data(), &mode_count,
                                modes.data(), nullptr);
    } while (rc == ERROR_INSUFFICIENT_BUFFER);
    if (rc == ERROR_SUCCESS) {
        paths.resize(path_count);
        for (const auto& path : paths) {
            DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
            source.header.adapterId = path.targetInfo.adapterId;
            source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
            source.header.size = sizeof(source);
            source.header.id = path.sourceInfo.id;
            if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) break;
            if (wcscmp(gdi_device, source.viewGdiDeviceName) != 0) continue;

            DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
            target.header.adapterId = path.targetInfo.adapterId;
            target.header.id = path.targetInfo.id;
            target.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
            target.header.size = sizeof(target);
            if (DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS) {
                std::string name = Utf8(target.monitorFriendlyDeviceName);
                if (!name.empty()) return name;
            }
            break;
        }
    }
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(gdi_device, 0, &device, 0)) return Utf8(device.DeviceString);
    return {};
}

DisplayMode ModeOf(const DEVMODEW& mode) {
    // 0 and 1 mean the hardware's default rate
    const double hz = mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
    return {static_cast<int>(mode.dmPelsWidth), static_cast<int>(mode.dmPelsHeight), hz};
}

struct Found {
    HMONITOR monitor = nullptr;
    std::wstring device;
    bool primary = false;
};

BOOL CALLBACK AddMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        reinterpret_cast<std::vector<Found>*>(data)->push_back(
            {monitor, info.szDevice, (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    }
    return TRUE;
}

}

std::vector<Monitor> ListMonitors(void* native_window) {
    std::vector<Found> found;
    EnumDisplayMonitors(nullptr, nullptr, AddMonitor, reinterpret_cast<LPARAM>(&found));
    const HMONITOR window_monitor =
        native_window ? MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTONEAREST)
                      : nullptr;

    std::vector<bool> primary;
    for (const auto& f : found) primary.push_back(f.primary);
    std::vector<Monitor> monitors;
    for (size_t i : SdlDisplayOrder(primary)) {
        const wchar_t* device = found[i].device.c_str();
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        // SDL leaves out a display whose mode it can't read
        if (!EnumDisplaySettingsW(device, ENUM_CURRENT_SETTINGS, &mode)) continue;
        Monitor monitor;
        monitor.name = MonitorName(device);
        monitor.primary = found[i].primary;
        monitor.has_window = window_monitor && found[i].monitor == window_monitor;
        monitor.current = ModeOf(mode);
        std::vector<DisplayMode> modes{monitor.current};
        for (DWORD n = 0;; n++) {
            DEVMODEW each{};
            each.dmSize = sizeof(each);
            if (!EnumDisplaySettingsW(device, n, &each)) break;
            // SDL has no palettized modes
            if (each.dmBitsPerPel <= 8) continue;
            modes.push_back(ModeOf(each));
        }
        monitor.modes = SortDisplayModes(std::move(modes));
        monitors.push_back(std::move(monitor));
    }
    return monitors;
}

#else

std::vector<Monitor> ListMonitors(void*) { return {}; }

#endif

}
