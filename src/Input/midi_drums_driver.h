#pragma once
#include <memory>
#include <string>
#include <vector>
#include <rex/input/input_driver.h>
#include "midi_drums.h"

// A MIDI drum kit (see midi_drums.h), read with RtMidi and reported to RB3 as an
// RB2 Xbox 360 drum kit on its own player slot. Turned on by the midi_drums
// setting; midi_drums_device picks the MIDI input port.

namespace band3::input {

std::unique_ptr<rex::input::InputDriver> CreateMidiDrumsDriver();

// the MIDI drums driver's kit
bool IsMidiDrums(const rex::input::DeviceInfo& device);

// Hands the running driver midi_drums_pulse_ms, midi_drums_min_velocity and
// midi_drums_combos as they are now, which it plays by from its next MIDI
// message on, without a restart: the launcher's slider changes them every
// frame of a drag. The other settings (the port, the notes) are read when the
// driver starts. UI thread; does nothing while no driver runs.
void UpdateMidiDrumsSettings();

// For the Instrument Lab.
struct MidiDrumsStatus {
    bool running = false;
    // the port being played, empty while none is open
    std::string port;
    // every MIDI input port
    std::vector<std::string> ports;
    // the most recent messages that were notes, newest last
    std::vector<midi_drums::Hit> recent;
    uint8_t min_velocity = 0;
};

MidiDrumsStatus GetMidiDrumsStatus();

}
