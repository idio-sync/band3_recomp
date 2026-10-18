#pragma once
#include <array>
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

// The RB3 keyboard and Pro Guitars, and the MIDI Pro Adapter in keys and guitar
// modes (PlasticBand Docs/Instruments/Pro Keyboard and Pro Guitar/PS3 and
// Wii.md). Report bytes 5 to 14 hold the same keys, frets, velocities and flags
// as an Xbox 360 one's left trigger to right stick, in the same order, so they
// are copied across; the face buttons and d-pad navigate as usual.
std::optional<Gamepad360> TranslatePs3ProInstrument(std::span<const uint8_t> report);

// PS3 keyboards and Pro Guitars only send their buttons until they get this
// 40-byte feature report (Wii ones send everything regardless). It is sent as
// the five 8-byte reports, report ID first, that the Pro Guitar's
// auto-calibration commands are known to work as; it shares their layout.
using Ps3EnableReport = std::array<uint8_t, 9>;
std::span<const Ps3EnableReport> Ps3ProEnableReports();

}
