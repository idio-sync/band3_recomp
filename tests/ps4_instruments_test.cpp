// Checks the PS4/PS5 instrument translation (src/Input/ps4_instruments.cpp).
// Reports are built from PlasticBand's documented layout (Docs/Base
// Reports/PS4.md, the PS4 and PS5 Rock Band guitar docs and the PS4 drum doc),
// and the output is checked against what RB3 does with an Xbox 360 instrument
// (rb3_reference.h).

#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <vector>
#include "rb3_reference.h"
#include "src/Input/hid_instrument_types.h"

using namespace band3::input;
using namespace rb3;

namespace {

// first button byte: hat switch in the low nibble, then the face buttons
constexpr uint8_t kSquare = 0x10;
constexpr uint8_t kCross = 0x20;
constexpr uint8_t kCircle = 0x40;
constexpr uint8_t kTriangle = 0x80;
// second button byte
constexpr uint8_t kL1 = 0x01;
constexpr uint8_t kR1 = 0x02;
constexpr uint8_t kShare = 0x10;
constexpr uint8_t kOptions = 0x20;
constexpr uint8_t kL3 = 0x40;

constexpr uint8_t kHatUp = 0;
constexpr uint8_t kHatDown = 4;

// where things are in each report, counting the report ID at byte 0
struct Layout {
    size_t buttons1, buttons2;
    uint8_t hat_centered;
};
constexpr Layout kPs4{5, 6, 8};
constexpr Layout kPs5{8, 9, 15};

// a 64-byte report at rest
struct Report {
    std::array<uint8_t, 64> bytes{};
    Layout layout;

    explicit Report(Layout l) : layout(l) {
        bytes[0] = 0x01;
        bytes[1] = bytes[2] = bytes[3] = bytes[4] = 0x80;
        bytes[layout.buttons1] = layout.hat_centered;
    }
    Report& Face(uint8_t bits) {
        bytes[layout.buttons1] = static_cast<uint8_t>((bytes[layout.buttons1] & 0x0F) | bits);
        return *this;
    }
    Report& Hat(uint8_t hat) {
        bytes[layout.buttons1] = static_cast<uint8_t>((bytes[layout.buttons1] & 0xF0) | hat);
        return *this;
    }
    Report& Buttons2(uint8_t bits) {
        bytes[layout.buttons2] = bits;
        return *this;
    }
    Report& Byte(size_t offset, uint8_t value) {
        bytes[offset] = value;
        return *this;
    }
};

Gamepad360 Translate(HidInstrumentType type, std::span<const uint8_t> report) {
    HidInstrumentTranslator t(type);
    auto g = t.Translate(report);
    REQUIRE(g.has_value());
    return *g;
}

Gamepad360 Translate(HidInstrumentType type, const Report& report) {
    return Translate(type, report.bytes);
}

// PS4 guitar bytes
constexpr size_t kPickup = 43, kWhammy = 44, kTilt = 45, kFrets = 46, kSoloFrets = 47;
// PS5 guitar bytes
constexpr size_t kPs5Pickup = 40, kPs5Whammy = 41, kPs5Tilt = 42, kPs5Frets = 43,
                 kPs5SoloFrets = 44;
// PS4 drum velocity bytes
constexpr size_t kRedPadAt = 43, kBluePadAt = 44, kYellowPadAt = 45, kGreenPadAt = 46,
                 kYellowCymbalAt = 47, kBlueCymbalAt = 48, kGreenCymbalAt = 49;

}

TEST_CASE("PS4 and PS5 instruments are identified by USB id") {
    CHECK(IdentifyHidInstrument(0x0738, 0x8261, 0) == HidInstrumentType::kPs4Guitar);
    CHECK(IdentifyHidInstrument(0x0E6F, 0x0173, 0) == HidInstrumentType::kPs4Guitar);
    CHECK(IdentifyHidInstrument(0x0E6F, 0x024A, 0x0101) == HidInstrumentType::kPs4Guitar);
    CHECK(IdentifyHidInstrument(0x3651, 0x5500, 0) == HidInstrumentType::kPs4Guitar);
    CHECK(IdentifyHidInstrument(0x0E6F, 0x0249, 0x0101) == HidInstrumentType::kPs5Guitar);
    CHECK(IdentifyHidInstrument(0x3958, 0x5600, 0) == HidInstrumentType::kPs5Guitar);
    CHECK(IdentifyHidInstrument(0x0738, 0x8262, 0) == HidInstrumentType::kPs4Drums);
    CHECK(IdentifyHidInstrument(0x0E6F, 0x0174, 0) == HidInstrumentType::kPs4Drums);
    // a DualShock 4
    CHECK_FALSE(IdentifyHidInstrument(0x054C, 0x09CC, 0).has_value());
}

TEST_CASE("PS4 guitar frets land on RB3's slots") {
    for (int fret = 0; fret < kFretCount; fret++) {
        const Gamepad360 g = Translate(HidInstrumentType::kPs4Guitar,
                                       Report(kPs4).Byte(kFrets, static_cast<uint8_t>(1 << fret)));
        CAPTURE(fret);
        CHECK(GuitarSlot(JoypadButtonFor(SlotButtons(g.buttons))) == fret);
        CHECK((g.buttons & kLeftThumb) == 0);
    }
}

TEST_CASE("PS4 guitar solo frets add RB3's shift button") {
    Gamepad360 g =
        Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kSoloFrets, 0x04));
    CHECK(g.buttons == (kY | kLeftThumb));

    // the Riffmaster's joystick click shares the solo flag, and isn't a solo fret
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Buttons2(kL3));
    CHECK(g.buttons == 0);
}

TEST_CASE("PS4 guitar strum, menu buttons, whammy, tilt and pickup") {
    Gamepad360 g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Hat(kHatUp));
    CHECK(g.buttons == kDpadUp);
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Hat(kHatDown));
    CHECK(g.buttons == kDpadDown);
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Buttons2(kShare | kOptions));
    CHECK(g.buttons == (kBack | kStart));

    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kWhammy, 0x00));
    CHECK(u16(g.thumb_rx) == 0x8000);
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kWhammy, 0xFF));
    CHECK(g.thumb_rx == 32767);

    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kTilt, 0xFF));
    CHECK(g.thumb_ry == 32767);
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kTilt, 0x00));
    CHECK(g.thumb_ry == 0);

    // notch 0-4 to the values an Xbox 360 guitar reports at each notch
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kPickup, 2));
    CHECK(g.left_trigger == 0x79);
    g = Translate(HidInstrumentType::kPs4Guitar, Report(kPs4).Byte(kPickup, 4));
    CHECK(g.left_trigger == 0xE0);
}

TEST_CASE("PS5 guitars use the DualSense button bytes") {
    for (int fret = 0; fret < kFretCount; fret++) {
        const Gamepad360 g = Translate(
            HidInstrumentType::kPs5Guitar,
            Report(kPs5).Byte(kPs5Frets, static_cast<uint8_t>(1 << fret)));
        CAPTURE(fret);
        CHECK(GuitarSlot(JoypadButtonFor(SlotButtons(g.buttons))) == fret);
    }
    Gamepad360 g = Translate(HidInstrumentType::kPs5Guitar,
                             Report(kPs5).Byte(kPs5SoloFrets, 0x01).Hat(kHatDown));
    CHECK(g.buttons == (kA | kLeftThumb | kDpadDown));
    g = Translate(HidInstrumentType::kPs5Guitar, Report(kPs5).Buttons2(kOptions));
    CHECK(g.buttons == kStart);
    // the PS5 hat switch rests at 15
    g = Translate(HidInstrumentType::kPs5Guitar, Report(kPs5));
    CHECK(g.buttons == 0);

    g = Translate(HidInstrumentType::kPs5Guitar,
                  Report(kPs5).Byte(kPs5Whammy, 0xFF).Byte(kPs5Tilt, 0xFF).Byte(kPs5Pickup, 1));
    CHECK(g.thumb_rx == 32767);
    CHECK(g.thumb_ry == 32767);
    CHECK(g.left_trigger == 0x4B);
}

TEST_CASE("PS4 reports without their report ID, and short reports") {
    Report r(kPs4);
    r.Byte(kFrets, 0x01);
    std::vector<uint8_t> without_id(r.bytes.begin() + 1, r.bytes.end());
    CHECK(Translate(HidInstrumentType::kPs4Guitar, without_id).buttons == kA);

    HidInstrumentTranslator t(HidInstrumentType::kPs4Guitar);
    std::vector<uint8_t> short_report(r.bytes.begin(), r.bytes.begin() + 40);
    CHECK_FALSE(t.Translate(short_report).has_value());
}

TEST_CASE("PS4 drum pads land on RB3's slots with the pad flag") {
    const std::pair<size_t, int> pads[] = {
        {kRedPadAt, 1}, {kYellowPadAt, 2}, {kBluePadAt, 3}, {kGreenPadAt, 4}};
    const uint8_t faces[] = {kCircle, kTriangle, kSquare, kCross};
    for (size_t i = 0; i < std::size(pads); i++) {
        const auto [offset, slot] = pads[i];
        // the kit presses the color's face button along with the velocity
        const Gamepad360 g = Translate(HidInstrumentType::kPs4Drums,
                                       Report(kPs4).Face(faces[i]).Byte(offset, 200));
        CAPTURE(slot);
        CHECK(DrumSlot(JoypadButtonFor(SlotButtons(g.buttons))) == slot);
        CHECK((g.buttons & kRightThumb) != 0);
        CHECK((g.buttons & kRightShoulder) == 0);
    }
}

TEST_CASE("PS4 cymbals become 360 cymbal hits") {
    Gamepad360 g = Translate(HidInstrumentType::kPs4Drums,
                             Report(kPs4).Face(kTriangle).Byte(kYellowCymbalAt, 100));
    CHECK(g.buttons == (kY | kRightShoulder | kDpadUp));
    g = Translate(HidInstrumentType::kPs4Drums,
                  Report(kPs4).Face(kSquare).Byte(kBlueCymbalAt, 100));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
    g = Translate(HidInstrumentType::kPs4Drums,
                  Report(kPs4).Face(kCross).Byte(kGreenCymbalAt, 100));
    CHECK(g.buttons == (kA | kRightShoulder));

    // a d-pad press during a cymbal hit is left to the cymbal markers
    g = Translate(HidInstrumentType::kPs4Drums,
                  Report(kPs4).Face(kSquare).Byte(kBlueCymbalAt, 100).Hat(kHatUp));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
}

TEST_CASE("PS4 drum face buttons, kicks and menu buttons") {
    // a face button with no velocity is a button press, without the pad flag
    Gamepad360 g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Face(kCircle));
    CHECK(g.buttons == kB);

    g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Buttons2(kL1));
    CHECK(DrumSlot(JoypadButtonFor(g.buttons)) == 0);  // kick
    g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Buttons2(kR1));
    CHECK(g.buttons == kLeftThumb);  // second kick
    g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Buttons2(kShare | kOptions));
    CHECK(g.buttons == (kBack | kStart));
    g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Hat(kHatUp));
    CHECK(g.buttons == kDpadUp);
}

TEST_CASE("PS4 drum velocities scale onto the 360 velocity axes") {
    // hardest hit: 255 here, 0 on the inverted 360 axis
    Gamepad360 g = Translate(HidInstrumentType::kPs4Drums,
                             Report(kPs4).Face(kCircle).Byte(kRedPadAt, 255));
    CHECK(g.thumb_lx == 0);
    // softest hit: 1 here, 0x7FFF on the 360 axis
    g = Translate(HidInstrumentType::kPs4Drums, Report(kPs4).Face(kCircle).Byte(kRedPadAt, 1));
    CHECK(u16(g.thumb_lx) == 0x7FFF);
}

TEST_CASE("PS4 instruments report as the matching 360 models") {
    const Caps360 guitar = HidInstrumentCaps(HidInstrumentType::kPs4Guitar);
    CHECK(guitar.sub_type == kSubtypeGuitar);
    CHECK_FALSE(SeesRb2Guitar(guitar));
    CHECK(HidInstrumentCaps(HidInstrumentType::kPs5Guitar).sub_type == kSubtypeGuitar);

    const Caps360 drums = HidInstrumentCaps(HidInstrumentType::kPs4Drums);
    CHECK(drums.sub_type == kSubtypeDrums);
    CHECK(SeesRb2Drums(drums));
    CHECK_FALSE(SeesMidiProAdapter(drums));
}
