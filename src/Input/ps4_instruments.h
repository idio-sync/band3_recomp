#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "instruments.h"

// PS4 and PS5 Rock Band guitars (MadCatz Stratocaster, PDP Jaguar, PDP
// Riffmaster, CRKD Gibson SG) and PS4 drum kits, translated to the Xbox 360
// instruments RB3 reads. hid_instrument_types.h says which devices use these.
//
// Report layout: PlasticBand Docs/Base Reports/PS4.md and
// Docs/Instruments/5-Fret Guitar/Rock Band/{PS4,PS5}.md and
// Docs/Instruments/4-Lane Drums/PS4.md, cross-checked against YARG's
// PlasticBand-Unity (PS4RockBandGuitar.cs, PS4RiffmasterGuitar.cs,
// PS5RiffmasterGuitar.cs, PS4FourLaneDrumKit.cs).
//
// Each takes a report as read, which starts with report ID 0x01 (a report
// without it is taken as having it removed), and returns nullopt if it is too
// short to be one.

namespace band3::input {

std::optional<Gamepad360> TranslatePs4Guitar(std::span<const uint8_t> report);
std::optional<Gamepad360> TranslatePs5Guitar(std::span<const uint8_t> report);
std::optional<Gamepad360> TranslatePs4Drums(std::span<const uint8_t> report);

}
