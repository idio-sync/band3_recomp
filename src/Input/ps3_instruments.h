#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "instruments.h"

// PS3 and Wii Rock Band guitars and drum kits (and the MIDI Pro Adapter in drum
// mode, which reports as a drum kit), translated to the Xbox 360 instruments
// RB3 reads.
//
// Report layout: PlasticBand Docs/Base Reports/PS3.md and
// Docs/Instruments/{5-Fret Guitar/Rock Band, 4-Lane Drums}/PS3 and Wii.md,
// cross-checked against YARG's PlasticBand-Unity (PS3RockBandGuitar.cs,
// PS3FourLaneDrumkit.cs). The two name the shoulder buttons differently, so the
// bits are named by what they do here.

namespace band3::input {

enum class Ps3Instrument {
    kGuitar,
    // RB1 kits send no pad/cymbal flags and no velocity
    kDrumsRb1,
    kDrums,
};

struct Ps3InstrumentId {
    uint16_t vendor;
    uint16_t product;
    const char* name;
};

// every device handled here
std::span<const Ps3InstrumentId> KnownPs3Instruments();

// which instrument a HID device is, from its USB ids and release number
std::optional<Ps3Instrument> IdentifyPs3Instrument(uint16_t vendor, uint16_t product,
                                                   uint16_t release);

// what the instrument reports itself as to RB3. Guitars read as RB1-style
// guitars, since the 360's auto-calibration sensors don't exist on them.
Caps360 Ps3InstrumentCaps(Ps3Instrument instrument);

// Turns one device's reports into Xbox 360 state. Kept per device: after a
// moment at rest the guitar's whammy and pickup switch report 0x7F, which means
// "unchanged" rather than a position.
class Ps3InstrumentTranslator {
public:
    explicit Ps3InstrumentTranslator(Ps3Instrument instrument) : instrument_(instrument) {}

    // a report as read, with or without a leading zero report ID; nullopt if
    // it is too short to be one
    std::optional<Gamepad360> Translate(std::span<const uint8_t> report);

private:
    Ps3Instrument instrument_;
    uint8_t whammy_ = 0;
    uint8_t pickup_ = 0;
};

}
