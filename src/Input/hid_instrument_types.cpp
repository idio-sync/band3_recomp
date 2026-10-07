#include "hid_instrument_types.h"
#include "gip_instruments.h"
#include "ps4_instruments.h"

namespace band3::input {

namespace {

using enum HidInstrumentType;

// ids from PlasticBand's device docs and PlasticBand-Unity's registrations
constexpr KnownHidInstrument kKnown[] = {
    {0x12BA, 0x0200, kPs3Guitar, "PS3 Rock Band guitar"},
    {0x12BA, 0x0210, kPs3Drums, "PS3 Rock Band drums"},
    {0x12BA, 0x0218, kPs3Drums, "PS3 MIDI Pro Adapter (drums)"},
    {0x1BAD, 0x0004, kPs3Guitar, "Wii Rock Band guitar"},
    {0x1BAD, 0x3010, kPs3Guitar, "Wii Rock Band 2 guitar"},
    {0x1BAD, 0x0005, kPs3DrumsRb1, "Wii Rock Band drums"},
    {0x1BAD, 0x3110, kPs3Drums, "Wii Rock Band 2 drums"},
    {0x1BAD, 0x3138, kPs3Drums, "Wii MIDI Pro Adapter (drums)"},

    {0x0738, 0x8261, kPs4Guitar, "PS4 MadCatz Stratocaster"},
    {0x0E6F, 0x0173, kPs4Guitar, "PS4 PDP Jaguar"},
    {0x0E6F, 0x024A, kPs4Guitar, "PDP Riffmaster (PS4)"},
    {0x3651, 0x1500, kPs4Guitar, "CRKD Gibson SG (PS4, wired)"},
    {0x3651, 0x5500, kPs4Guitar, "CRKD Gibson SG (PS4, dongle)"},
    {0x3958, 0x5500, kPs4Guitar, "CRKD Gibson SG (PS4, dongle)"},
    {0x0E6F, 0x0249, kPs5Guitar, "PDP Riffmaster (PS5)"},
    {0x3651, 0x1600, kPs5Guitar, "CRKD Gibson SG (PS5, wired)"},
    {0x3651, 0x5600, kPs5Guitar, "CRKD Gibson SG (PS5, dongle)"},
    {0x3958, 0x5600, kPs5Guitar, "CRKD Gibson SG (PS5, dongle)"},
    {0x0738, 0x8262, kPs4Drums, "PS4 MadCatz drums"},
    {0x0E6F, 0x0174, kPs4Drums, "PS4 PDP drums"},
};

// PlasticBand's Xbox One guitar and drum docs, RB4InstrumentMapper's
// MapperFactory.cs, and SDL's usb_ids.h for the Red Octane Games (CRKD) ones
constexpr KnownHidInstrument kKnownGip[] = {
    {0x0738, 0x4161, kXboxOneGuitar, "Xbox One MadCatz Stratocaster"},
    {0x0E6F, 0x0170, kXboxOneGuitar, "Xbox One PDP Jaguar"},
    {0x0E6F, 0x0248, kXboxOneGuitar, "PDP Riffmaster (Xbox One)"},
    {0x3651, 0x4161, kXboxOneGuitar, "CRKD guitar (Xbox One)"},
    {0x3958, 0x4161, kXboxOneGuitar, "CRKD guitar (Xbox One)"},
    {0x0738, 0x4262, kXboxOneDrums, "Xbox One MadCatz drums"},
    {0x0E6F, 0x0171, kXboxOneDrums, "Xbox One PDP drums"},
    {0x3958, 0xD300, kXboxOneDrums, "Red Octane drums (Xbox One)"},
};

}

std::span<const KnownHidInstrument> KnownHidInstruments() { return kKnown; }

std::span<const KnownHidInstrument> KnownGipInstruments() { return kKnownGip; }

std::optional<HidInstrumentType> IdentifyGipInstrument(uint16_t vendor, uint16_t product) {
    for (const auto& known : kKnownGip) {
        if (known.vendor == vendor && known.product == product) return known.type;
    }
    return std::nullopt;
}

std::optional<HidInstrumentType> IdentifyInstrument(uint16_t vendor, uint16_t product,
                                                    uint16_t release) {
    if (auto hid = IdentifyHidInstrument(vendor, product, release)) return hid;
    return IdentifyGipInstrument(vendor, product);
}

std::optional<HidInstrumentType> IdentifyHidInstrument(uint16_t vendor, uint16_t product,
                                                       uint16_t release) {
    for (const auto& known : kKnown) {
        if (known.vendor != vendor || known.product != product) continue;
        // RB1 PS3 kits share the RB2 kit's id and report release 0x1000
        // (RB2 ones report 0x0200)
        if (known.vendor == 0x12BA && known.product == 0x0210 && release == 0x1000) {
            return kPs3DrumsRb1;
        }
        return known.type;
    }
    return std::nullopt;
}

const char* HidInstrumentTypeLabel(HidInstrumentType type) {
    switch (type) {
    case kPs3Guitar: return "a PS3/Wii guitar";
    case kPs3DrumsRb1: return "an RB1 PS3/Wii drum kit (no cymbals or velocity)";
    case kPs3Drums: return "a PS3/Wii drum kit";
    case kPs4Guitar: return "a PS4 guitar";
    case kPs5Guitar: return "a PS5 guitar";
    case kPs4Drums: return "a PS4 drum kit";
    case kXboxOneGuitar: return "an Xbox One guitar";
    case kXboxOneDrums: return "an Xbox One drum kit";
    }
    return "an instrument";
}

Caps360 HidInstrumentCaps(HidInstrumentType type) {
    switch (type) {
    case kPs3Guitar:
    case kPs4Guitar:
    case kPs5Guitar:
    // its auto-calibration sensors stay off until asked for, and band3 doesn't ask
    case kXboxOneGuitar: return GuitarCaps(false);
    case kPs3DrumsRb1: return DrumCaps(false);
    case kPs3Drums:
    case kPs4Drums:
    case kXboxOneDrums: return DrumCaps(true);
    }
    return GuitarCaps(false);
}

std::optional<Gamepad360> HidInstrumentTranslator::Translate(std::span<const uint8_t> report) {
    switch (type_) {
    case kPs3Guitar: return TranslatePs3Guitar(report, ps3_guitar_);
    case kPs3DrumsRb1: return TranslatePs3Drums(report, false);
    case kPs3Drums: return TranslatePs3Drums(report, true);
    case kPs4Guitar: return TranslatePs4Guitar(report);
    case kPs5Guitar: return TranslatePs5Guitar(report);
    case kPs4Drums: return TranslatePs4Drums(report);
    case kXboxOneGuitar: return TranslateXboxOneGuitar(report);
    case kXboxOneDrums: return TranslateXboxOneDrums(report);
    }
    return std::nullopt;
}

}
