#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include "instruments.h"
#include "ps3_instruments.h"

// Every HID instrument band3 reads (see hid_instruments.h): which USB ids are
// which instrument, what each reports itself as to RB3, and how its reports
// are translated.

namespace band3::input {

enum class HidInstrumentType {
    kPs3Guitar,     // PS3 and Wii guitars
    kPs3DrumsRb1,   // RB1 PS3 and Wii kits: no pad/cymbal flags, no velocity
    kPs3Drums,      // RB2-and-later PS3 and Wii kits, the MIDI Pro Adapter in drum mode
    kPs4Guitar,     // PS4 guitars, and the Riffmaster and CRKD SG in PS4 mode
    kPs5Guitar,     // the Riffmaster and CRKD SG in PS5 mode
    kPs4Drums,
};

struct KnownHidInstrument {
    uint16_t vendor;
    uint16_t product;
    HidInstrumentType type;
    const char* name;
};

std::span<const KnownHidInstrument> KnownHidInstruments();

// which instrument a HID device is, from its USB ids and release number
std::optional<HidInstrumentType> IdentifyHidInstrument(uint16_t vendor, uint16_t product,
                                                       uint16_t release);

// for the Instrument Lab, e.g. "a PS4 guitar"
const char* HidInstrumentTypeLabel(HidInstrumentType type);

// What the instrument reports itself as to RB3. Guitars read as RB1-style
// guitars, since none of these has the 360's auto-calibration sensors.
Caps360 HidInstrumentCaps(HidInstrumentType type);

// Turns one device's reports into Xbox 360 state; keeps what little state a
// device needs between reports.
class HidInstrumentTranslator {
public:
    explicit HidInstrumentTranslator(HidInstrumentType type) : type_(type) {}

    // a report as read; nullopt if it is too short to be one
    std::optional<Gamepad360> Translate(std::span<const uint8_t> report);

private:
    HidInstrumentType type_;
    Ps3GuitarState ps3_guitar_;
};

}
