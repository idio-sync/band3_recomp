#pragma once
#include <string>
#include <vector>
#include "device_names.h"

// The launcher's lists of the devices on this PC, for its dropdowns. Each call
// looks again, and some take a while (SDL starts its audio subsystem for the
// microphones), so call them every second or two rather than every frame.
// UI thread, before the game starts.

namespace band3::launcher {

// The recording devices band3's SDL lists, which the game's mic capture picks
// from by usb_mic_devices. Read straight from SDL: the game's capture can't be
// started before the game, nor restarted after.
std::vector<std::string> RecordingDeviceNames();

struct MidiPort {
    // what midi_drums_device saves and the dropdown shows: RtMidi's name
    // without WinMM's port index (StripMidiPortIndex)
    std::string name;
    // RtMidi's name, which FindMidiPort matches as the driver does
    std::string port;
};

// The MIDI input ports, from RtMidi. Only lists them, so it works while the
// MIDI drums driver has one open (WinMM lets one program open a port at once).
std::vector<MidiPort> MidiInputPorts();

struct Monitor {
    std::string name;
    bool primary = false;
    // the desktop's mode, and every fullscreen mode (SortDisplayModes' order)
    DisplayMode current;
    std::vector<DisplayMode> modes;
};

// The monitors in the order the SDK's monitor setting numbers them: entry i is
// monitor = i + 1 (0 is the default display). Named and with their modes from
// Win32 in SDL's order (SdlDisplayOrder) on Windows. Empty elsewhere: band3's
// SDL would have to start its video subsystem next to the SDK's, which isn't
// known to be safe, so the launcher numbers the monitors there instead.
std::vector<Monitor> ListMonitors();

}
