#pragma once

// What an input device is, for the launcher's device list (PlayerDevices in
// input_system.h). Kept apart from input_system.h, which needs the SDK, so the
// unit tests can use it.

namespace band3::input {

enum class DeviceKind {
    kPad,            // an SDL or XInput controller (an Xbox instrument too)
    kSynthetic,      // the keyboard and mouse, or the SDK's stand-in
    kVirtual,        // a virtual instrument (debug)
    kHidInstrument,  // a PS3/Wii/PS4/PS5 instrument read by its dongle
    kMidiDrums,      // a MIDI drum kit
    kMidiKeys,       // a MIDI keyboard, played as a keytar
    kSdlCopy,        // SDL's copy of an instrument the HID driver reads; no player
    kStageKit,       // a Stage Kit, which band3 lights (src/Lights/); no player
};

}
