#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "instruments.h"

// Xbox One Rock Band guitars (MadCatz Stratocaster, PDP Jaguar and Riffmaster,
// CRKD) and drum kits, translated to the Xbox 360 instruments RB3 reads.
// hid_instrument_types.h says which devices use these.
//
// These speak Microsoft's GIP rather than HID; band3 reads their raw reports
// through GameInput (gameinput_instruments.h), which hands over a report's ID
// and its data apart. A report here is the ID followed by the data, as band3
// records it: 0x20 is the input state, and anything else is ignored.
//
// Report layout: PlasticBand Docs/Instruments/5-Fret Guitar/Rock Band/Xbox
// One.md and Docs/Instruments/4-Lane Drums/Xbox One.md, cross-checked against
// RB4InstrumentMapper (XboxGuitarInput.cs, XboxDrumInput.cs and their ViGEm
// mappers) and SDL's GameInput driver (SDL_gameinputjoystick.cpp).
//
// Each returns nullopt for a report that isn't an input state, or is too short
// to be one.

namespace band3::input {

// the ID of a GIP input state report
inline constexpr uint8_t kGipInputReport = 0x20;

std::optional<Gamepad360> TranslateXboxOneGuitar(std::span<const uint8_t> report);
std::optional<Gamepad360> TranslateXboxOneDrums(std::span<const uint8_t> report);

}
