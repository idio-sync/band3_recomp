#pragma once
#include <memory>
#include <rex/system/interfaces/input.h>

namespace band3::input {

// The SDK's input system (SDL or XInput, plus keyboard/mouse) with band3's
// drivers added: the Instrument Lab's virtual instrument and, with the
// hid_instruments and midi_drums settings on, instrument dongles and MIDI drum
// kits. Player slots make room for the
// virtual instrument. For RuntimeConfig::input_factory.
std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode);

}
