#pragma once
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>
#include <rex/input/input_driver.h>
#include "midi_keys.h"

// A MIDI keyboard (see midi_keys.h), read with RtMidi and reported to RB3 as an
// Xbox 360 keytar on its own player slot. Turned on by the midi_keys setting;
// midi_keys_device picks the MIDI input port, and midi_keys_base_note, read as
// it plays, which note is the keytar's lowest C.

namespace band3::input {

std::unique_ptr<rex::input::InputDriver> CreateMidiKeysDriver();

// the MIDI keyboard driver's keyboard
bool IsMidiKeys(const rex::input::DeviceInfo& device);

// For the Instrument Lab and the launcher.
struct MidiKeysStatus {
    bool running = false;
    // the port being played, empty while none is open ("harness" for the test
    // harness's keyboard, midi_keys_test_device)
    std::string port;
    // every MIDI input port
    std::vector<std::string> ports;
    // what the most recent messages did, newest last, up to 12
    std::vector<midi_keys::Event> recent;
    // what a note-on means now
    midi_keys::Mode mode = midi_keys::Mode::kMenus;
    uint8_t base_note = 48;
};

MidiKeysStatus GetMidiKeysStatus();

// The test harness's `midi` command: hands the running driver a message as its
// port would. False when no driver runs. With midi_keys_test_device (and the
// harness on), the first message connects the harness's keyboard if no port is
// open, so a script can free a player for it first.
bool InjectMidiKeysMessage(std::span<const uint8_t> message);

}
