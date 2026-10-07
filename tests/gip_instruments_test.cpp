// Checks the Xbox One instrument translation (src/Input/gip_instruments.cpp).
// Reports are built from PlasticBand's documented layout (Docs/Instruments/
// 5-Fret Guitar/Rock Band/Xbox One.md and 4-Lane Drums/Xbox One.md), led by
// the report ID as band3 records it, and the output is checked against what
// RB3 does with an Xbox 360 instrument (rb3_reference.h).

#include <doctest/doctest.h>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>
#include "rb3_reference.h"
#include "src/Input/gip_instruments.h"
#include "src/Input/hid_instrument_types.h"

using namespace band3::input;
using namespace rb3;

namespace {

// offsets count the report ID at byte 0
// first button byte
constexpr uint8_t kMenu = 0x04;
constexpr uint8_t kView = 0x08;
constexpr uint8_t kGreenFlag = 0x10;  // drums: A
constexpr uint8_t kRedFlag = 0x20;    // drums: B
constexpr uint8_t kBlueFlag = 0x40;   // drums: X
constexpr uint8_t kYellowFlag = 0x80;  // drums: Y
// second button byte
constexpr uint8_t kUp = 0x01;
constexpr uint8_t kDown = 0x02;
constexpr uint8_t kRight = 0x08;
constexpr uint8_t kKick1 = 0x10;  // a guitar's orange fret flag
constexpr uint8_t kKick2 = 0x20;
constexpr uint8_t kSoloFlag = 0x40;  // the Riffmaster's joystick click too

constexpr size_t kButtons1 = 1, kButtons2 = 2;
// guitar
constexpr size_t kTilt = 3, kWhammy = 4, kPickup = 5, kFrets = 6, kSoloFrets = 7;
// drums: velocity nibbles
constexpr size_t kYellowRedPadsAt = 3, kGreenBluePadsAt = 4, kBlueYellowCymbalsAt = 5,
                 kGreenCymbalAt = 6;

struct Report {
    std::vector<uint8_t> bytes;

    explicit Report(size_t data_size) : bytes(1 + data_size) { bytes[0] = kGipInputReport; }
    Report& Byte(size_t offset, uint8_t value) {
        bytes[offset] = value;
        return *this;
    }
};

Report Guitar() { return Report(10); }
Report Riffmaster() { return Report(28); }
Report Drums() { return Report(6); }

Gamepad360 Translate(HidInstrumentType type, const Report& report) {
    HidInstrumentTranslator t(type);
    auto g = t.Translate(report.bytes);
    REQUIRE(g.has_value());
    return *g;
}

Gamepad360 GuitarState(const Report& report) {
    return Translate(HidInstrumentType::kXboxOneGuitar, report);
}
Gamepad360 DrumState(const Report& report) {
    return Translate(HidInstrumentType::kXboxOneDrums, report);
}

}

TEST_CASE("Xbox One instruments are identified by USB id") {
    CHECK(IdentifyGipInstrument(0x0738, 0x4161) == HidInstrumentType::kXboxOneGuitar);
    CHECK(IdentifyGipInstrument(0x0E6F, 0x0170) == HidInstrumentType::kXboxOneGuitar);
    CHECK(IdentifyGipInstrument(0x0E6F, 0x0248) == HidInstrumentType::kXboxOneGuitar);
    CHECK(IdentifyGipInstrument(0x3651, 0x4161) == HidInstrumentType::kXboxOneGuitar);
    CHECK(IdentifyGipInstrument(0x0738, 0x4262) == HidInstrumentType::kXboxOneDrums);
    CHECK(IdentifyGipInstrument(0x0E6F, 0x0171) == HidInstrumentType::kXboxOneDrums);
    // the RB4 wireless legacy adapter and an Xbox One controller aren't read
    CHECK_FALSE(IdentifyGipInstrument(0x0738, 0x4164).has_value());
    CHECK_FALSE(IdentifyGipInstrument(0x045E, 0x02EA).has_value());
    // the PS4 Jaguar isn't the Xbox One one
    CHECK_FALSE(IdentifyGipInstrument(0x0E6F, 0x0173).has_value());
}

TEST_CASE("Xbox One and HID ids don't overlap, so a capture's ids say which it is") {
    for (const auto& gip : KnownGipInstruments()) {
        CAPTURE(gip.name);
        CHECK_FALSE(IdentifyHidInstrument(gip.vendor, gip.product, 0).has_value());
        CHECK(IdentifyInstrument(gip.vendor, gip.product, 0) == gip.type);
    }
    for (const auto& hid : KnownHidInstruments()) {
        CAPTURE(hid.name);
        CHECK_FALSE(IdentifyGipInstrument(hid.vendor, hid.product).has_value());
    }
}

TEST_CASE("Xbox One guitar frets land on RB3's slots") {
    for (int fret = 0; fret < kFretCount; fret++) {
        const Gamepad360 g = GuitarState(Guitar().Byte(kFrets, static_cast<uint8_t>(1 << fret)));
        CAPTURE(fret);
        CHECK(GuitarSlot(JoypadButtonFor(SlotButtons(g.buttons))) == fret);
        CHECK((g.buttons & kLeftThumb) == 0);
    }
}

TEST_CASE("Xbox One guitar solo frets add RB3's shift button; the fret flags don't count") {
    Gamepad360 g = GuitarState(Guitar().Byte(kSoloFrets, 0x04));
    CHECK(g.buttons == (kY | kLeftThumb));

    // the flags go along with the fret bytes, but the Riffmaster's joystick
    // click shares the solo flag, so only the fret bytes are read
    g = GuitarState(Guitar().Byte(kButtons2, kSoloFlag));
    CHECK(g.buttons == 0);
    g = GuitarState(Guitar().Byte(kButtons1, kGreenFlag | kRedFlag).Byte(kButtons2, kKick1));
    CHECK(g.buttons == 0);
}

TEST_CASE("Xbox One guitar strum, menu buttons, whammy, tilt and pickup") {
    Gamepad360 g = GuitarState(Guitar().Byte(kButtons2, kUp));
    CHECK(g.buttons == kDpadUp);
    g = GuitarState(Guitar().Byte(kButtons2, kDown));
    CHECK(g.buttons == kDpadDown);
    g = GuitarState(Guitar().Byte(kButtons2, kRight));
    CHECK(g.buttons == kDpadRight);
    g = GuitarState(Guitar().Byte(kButtons1, kMenu | kView));
    CHECK(g.buttons == (kStart | kBack));

    g = GuitarState(Guitar().Byte(kWhammy, 0x00));
    CHECK(u16(g.thumb_rx) == 0x8000);
    g = GuitarState(Guitar().Byte(kWhammy, 0xFF));
    CHECK(g.thumb_rx == 32767);

    g = GuitarState(Guitar().Byte(kTilt, 0xFF));
    CHECK(g.thumb_ry == 32767);
    g = GuitarState(Guitar().Byte(kTilt, 0x00));
    CHECK(g.thumb_ry == 0);

    // notch 0-4 in the high nibble, to the values an Xbox 360 guitar reports
    g = GuitarState(Guitar().Byte(kPickup, 0x00));
    CHECK(g.left_trigger == 0x17);
    g = GuitarState(Guitar().Byte(kPickup, 0x20));
    CHECK(g.left_trigger == 0x79);
    g = GuitarState(Guitar().Byte(kPickup, 0x40));
    CHECK(g.left_trigger == 0xE0);
}

TEST_CASE("the Riffmaster's longer report reads the same") {
    const Gamepad360 g =
        GuitarState(Riffmaster().Byte(kFrets, 0x01).Byte(kButtons2, kDown).Byte(12, 0x7F));
    CHECK(g.buttons == (kA | kDpadDown));
}

TEST_CASE("Xbox One reports other than the input state, and short ones, are ignored") {
    HidInstrumentTranslator guitar(HidInstrumentType::kXboxOneGuitar);
    Report other = Guitar();
    other.bytes[0] = 0x22;
    CHECK_FALSE(guitar.Translate(other.bytes).has_value());
    const Report full = Guitar();
    CHECK_FALSE(guitar.Translate(std::span(full.bytes).first(kSoloFrets)).has_value());
    CHECK(guitar.Translate(std::span(full.bytes).first(kSoloFrets + 1)).has_value());

    HidInstrumentTranslator drums(HidInstrumentType::kXboxOneDrums);
    const Report kit = Drums();
    CHECK_FALSE(drums.Translate(std::span(kit.bytes).first(kGreenCymbalAt)).has_value());
    CHECK_FALSE(drums.Translate(std::vector<uint8_t>{}).has_value());
}

TEST_CASE("Xbox One drum pads land on RB3's slots with the pad flag") {
    // offset, nibble shift, the face button the kit presses with it, RB3's slot
    struct Pad {
        size_t offset;
        int shift;
        uint8_t face;
        int slot;
    };
    const Pad pads[] = {{kYellowRedPadsAt, 4, kRedFlag, 1},
                        {kYellowRedPadsAt, 0, kYellowFlag, 2},
                        {kGreenBluePadsAt, 4, kBlueFlag, 3},
                        {kGreenBluePadsAt, 0, kGreenFlag, 4}};
    for (const Pad& pad : pads) {
        const Gamepad360 g = DrumState(Drums()
                                           .Byte(kButtons1, pad.face)
                                           .Byte(pad.offset, static_cast<uint8_t>(7 << pad.shift)));
        CAPTURE(pad.slot);
        CHECK(DrumSlot(JoypadButtonFor(SlotButtons(g.buttons))) == pad.slot);
        CHECK((g.buttons & kRightThumb) != 0);
        CHECK((g.buttons & kRightShoulder) == 0);
    }
}

TEST_CASE("Xbox One cymbals become 360 cymbal hits") {
    Gamepad360 g = DrumState(Drums().Byte(kBlueYellowCymbalsAt, 0x70));
    CHECK(g.buttons == (kY | kRightShoulder | kDpadUp));
    g = DrumState(Drums().Byte(kBlueYellowCymbalsAt, 0x07));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
    g = DrumState(Drums().Byte(kGreenCymbalAt, 0x70).Byte(kButtons1, kGreenFlag));
    CHECK(g.buttons == (kA | kRightShoulder));

    // a d-pad press during a cymbal hit is left to the cymbal markers
    g = DrumState(Drums().Byte(kBlueYellowCymbalsAt, 0x07).Byte(kButtons2, kUp));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
}

TEST_CASE("Xbox One drum face buttons, kicks and menu buttons") {
    // a face button with no velocity is a button press, without the pad flag
    Gamepad360 g = DrumState(Drums().Byte(kButtons1, kRedFlag));
    CHECK(g.buttons == kB);

    g = DrumState(Drums().Byte(kButtons2, kKick1));
    CHECK(DrumSlot(JoypadButtonFor(g.buttons)) == 0);  // kick
    g = DrumState(Drums().Byte(kButtons2, kKick2));
    CHECK(g.buttons == kLeftThumb);  // second kick
    g = DrumState(Drums().Byte(kButtons1, kMenu | kView));
    CHECK(g.buttons == (kStart | kBack));
    g = DrumState(Drums().Byte(kButtons2, kUp));
    CHECK(g.buttons == kDpadUp);
}

TEST_CASE("Xbox One drum velocities scale onto the 360 velocity axes") {
    // the 360 axis is inverted: harder hits read lower
    const auto red_axis = [](uint8_t nibble) {
        return u16(DrumState(Drums().Byte(kYellowRedPadsAt, static_cast<uint8_t>(nibble << 4)))
                       .thumb_lx);
    };
    CHECK(red_axis(15) == 0);
    CHECK(red_axis(1) > red_axis(7));
    CHECK(red_axis(7) > red_axis(15));
    // a soft hit is still a hit
    const Gamepad360 g = DrumState(Drums().Byte(kYellowRedPadsAt, 0x10));
    CHECK((g.buttons & (kB | kRightThumb)) == (kB | kRightThumb));
}

TEST_CASE("Xbox One instruments report as the matching 360 models") {
    const Caps360 guitar = HidInstrumentCaps(HidInstrumentType::kXboxOneGuitar);
    CHECK(guitar.sub_type == kSubtypeGuitar);
    CHECK_FALSE(SeesRb2Guitar(guitar));

    const Caps360 drums = HidInstrumentCaps(HidInstrumentType::kXboxOneDrums);
    CHECK(drums.sub_type == kSubtypeDrums);
    CHECK(SeesRb2Drums(drums));
    CHECK_FALSE(SeesMidiProAdapter(drums));
}
