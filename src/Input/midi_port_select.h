#pragma once
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

// Which MIDI input port a band3 MIDI device opens (midi_port.h), kept apart
// from RtMidi so the unit tests and the launcher can use it.

namespace band3::input {

// The port to open, from RtMidi's full port names. With `wanted`, the first
// whose name contains it, ignoring case, even if another band3 MIDI device holds
// it: the player named it. Without, the first that isn't a "through" loopback
// (Linux's "Midi Through") and isn't in `taken`, the ports band3's other MIDI
// devices hold, so a drum kit and a keyboard with no port named don't both take
// the first one.
std::optional<size_t> PickPort(const std::vector<std::string>& ports, const std::string& wanted,
                               const std::vector<std::string>& taken);

}
