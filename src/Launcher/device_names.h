#pragma once
#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The pure parts of the launcher's device lists (device_lists.h): how a saved
// setting picks an entry, how names are saved, and the order monitors are
// numbered in. Kept apart from the SDL, RtMidi and Win32 code so the unit
// tests can check them.

namespace band3::launcher {

// The entry a saved device setting (usb_mic_devices' slot, midi_drums_device)
// selects: the first whose name contains it, ignoring case, as the game's
// matching does. nullopt when it's empty or nothing matches: then the setting
// is shown as "<saved> (not connected)".
std::optional<size_t> FindSavedDevice(std::span<const std::string> names, std::string_view saved);

// The MIDI port the MIDI drums driver opens for midi_drums_device, as it picks
// it (midi_drums_driver.cpp): an empty setting takes the first port that isn't
// Linux's "Midi Through" loopback. Pass RtMidi's full port names.
std::optional<size_t> FindMidiPort(std::span<const std::string> ports, std::string_view saved);

// RtMidi's Windows (WinMM) backend names input port `index` "<device> <index>"
// (MidiInWinMM::getPortName), and the index moves as devices come and go, so
// midi_drums_device is saved without it. The name is left as it is unless it
// ends in " <index>". The saved name is still part of the full one, so the
// driver's matching still finds it.
std::string StripMidiPortIndex(std::string_view port, unsigned index);

// What a mic slot saves for a device, for JoinMicSlots (launcher_settings.h):
// its full name, unless the name has a comma, which would split
// usb_mic_devices' list: then its longest part without one, which still
// matches it.
std::string MicSlotValue(std::string_view device);

// The order SDL numbers displays in on Windows, which the SDK's monitor
// setting counts in (monitor N is SDL_GetDisplays()[N - 1]): SDL_windowsmodes.c
// WIN_AddDisplays enumerates the monitors twice, taking the primary one first
// and then the rest in EnumDisplayMonitors' order. `primary` holds, in
// EnumDisplayMonitors' order, whether each monitor is the primary one; the
// result is their indices in SDL's order.
std::vector<size_t> SdlDisplayOrder(const std::vector<bool>& primary);

struct DisplayMode {
    int width = 0;
    int height = 0;
    // 0 when unknown
    double refresh_hz = 0;
    bool operator==(const DisplayMode&) const = default;
};

// a display's modes as a list to pick from: each size and refresh rate once,
// largest first and the fastest refresh first within a size, as
// SDL_GetFullscreenDisplayModes sorts them
std::vector<DisplayMode> SortDisplayModes(std::vector<DisplayMode> modes);

}
