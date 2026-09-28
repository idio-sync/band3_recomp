#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "instruments.h"

// PS3 and Wii Rock Band guitars and drum kits (and the MIDI Pro Adapter in drum
// mode, which reports as a drum kit), translated to the Xbox 360 instruments
// RB3 reads. hid_instrument_types.h says which devices use these.
//
// Report layout: PlasticBand Docs/Base Reports/PS3.md and
// Docs/Instruments/{5-Fret Guitar/Rock Band, 4-Lane Drums}/PS3 and Wii.md,
// cross-checked against YARG's PlasticBand-Unity (PS3RockBandGuitar.cs,
// PS3FourLaneDrumkit.cs). The two name the shoulder buttons differently, so the
// bits are named by what they do here.

namespace band3::input {

// After a moment at rest the guitar's whammy and pickup switch report 0x7F,
// which means "unchanged" rather than a position, so their last real values are
// kept here, one per device.
struct Ps3GuitarState {
    uint8_t whammy = 0;
    uint8_t pickup = 0;
};

// Each takes a report as read, with or without a leading zero report ID, and
// returns nullopt if it is too short to be one.
std::optional<Gamepad360> TranslatePs3Guitar(std::span<const uint8_t> report,
                                             Ps3GuitarState& state);
// has_velocity is false for RB1 kits, which send no velocity
std::optional<Gamepad360> TranslatePs3Drums(std::span<const uint8_t> report, bool has_velocity);

}
