// Checks the PS3/Wii instrument translation (src/Input/ps3_instruments.cpp).
// Reports are built from PlasticBand's documented layout (Docs/Base Reports/PS3.md
// and the PS3 and Wii guitar and drum docs), and the output is checked against
// what RB3 does with an Xbox 360 instrument (rb3_reference.h).

#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <vector>
#include "rb3_reference.h"
#include "src/Input/ps3_instruments.h"

using namespace band3::input;
using namespace rb3;

namespace {

// PS3 report button bits, by position (PlasticBand's docs and PlasticBand-Unity
// name bits 4-7 differently, so they're numbered here)
constexpr uint16_t kSquare = 0x0001;
constexpr uint16_t kCross = 0x0002;
constexpr uint16_t kCircle = 0x0004;
constexpr uint16_t kTriangle = 0x0008;
constexpr uint16_t kBit4 = 0x0010;
constexpr uint16_t kBit5 = 0x0020;
constexpr uint16_t kBit6 = 0x0040;
constexpr uint16_t kSelect = 0x0100;
constexpr uint16_t kStartPs3 = 0x0200;
constexpr uint16_t kL3 = 0x0400;
constexpr uint16_t kR3 = 0x0800;

constexpr uint8_t kHatUp = 0;
constexpr uint8_t kHatUpRight = 1;
constexpr uint8_t kHatDown = 4;
constexpr uint8_t kHatCentered = 8;

// a 27-byte report at rest: nothing pressed, hat centered, sticks centered
struct Report {
    std::array<uint8_t, 27> bytes{};

    Report() {
        bytes[2] = kHatCentered;
        bytes[3] = bytes[4] = bytes[5] = bytes[6] = 0x7F;
    }
    Report& Buttons(uint16_t bits) {
        bytes[0] = static_cast<uint8_t>(bits);
        bytes[1] = static_cast<uint8_t>(bits >> 8);
        return *this;
    }
    Report& Hat(uint8_t hat) {
        bytes[2] = hat;
        return *this;
    }
    Report& Byte(size_t offset, uint8_t value) {
        bytes[offset] = value;
        return *this;
    }
};

Gamepad360 Translate(Ps3Instrument instrument, const Report& report) {
    Ps3InstrumentTranslator t(instrument);
    auto g = t.Translate(report.bytes);
    REQUIRE(g.has_value());
    return *g;
}

}

TEST_CASE("PS3 and Wii instruments are identified by USB id") {
    CHECK(IdentifyPs3Instrument(0x12BA, 0x0200, 0x0313) == Ps3Instrument::kGuitar);
    CHECK(IdentifyPs3Instrument(0x1BAD, 0x0004, 0x0100) == Ps3Instrument::kGuitar);
    CHECK(IdentifyPs3Instrument(0x1BAD, 0x3010, 0x0200) == Ps3Instrument::kGuitar);

    // RB1 PS3 kits report release 0x1000, RB2 ones 0x0200
    CHECK(IdentifyPs3Instrument(0x12BA, 0x0210, 0x1000) == Ps3Instrument::kDrumsRb1);
    CHECK(IdentifyPs3Instrument(0x12BA, 0x0210, 0x0200) == Ps3Instrument::kDrums);
    CHECK(IdentifyPs3Instrument(0x1BAD, 0x0005, 0x0100) == Ps3Instrument::kDrumsRb1);
    CHECK(IdentifyPs3Instrument(0x1BAD, 0x3110, 0x0200) == Ps3Instrument::kDrums);

    // MIDI Pro Adapter in drum mode
    CHECK(IdentifyPs3Instrument(0x12BA, 0x0218, 0) == Ps3Instrument::kDrums);
    CHECK(IdentifyPs3Instrument(0x1BAD, 0x3138, 0) == Ps3Instrument::kDrums);

    // Guitar Hero guitar, DualShock 3
    CHECK_FALSE(IdentifyPs3Instrument(0x12BA, 0x0100, 0).has_value());
    CHECK_FALSE(IdentifyPs3Instrument(0x054C, 0x0268, 0).has_value());

    for (const auto& known : KnownPs3Instruments()) {
        CAPTURE(known.name);
        CHECK(IdentifyPs3Instrument(known.vendor, known.product, 0).has_value());
    }
}

TEST_CASE("PS3 guitar frets land on RB3's slots") {
    const std::pair<uint16_t, int> frets[] = {
        {kCross, 0}, {kCircle, 1}, {kTriangle, 2}, {kSquare, 3}, {kBit4, 4}};
    for (const auto& [bit, slot] : frets) {
        const Gamepad360 g = Translate(Ps3Instrument::kGuitar, Report().Buttons(bit));
        CAPTURE(slot);
        CHECK(GuitarSlot(JoypadButtonFor(SlotButtons(g.buttons))) == slot);
    }
}

TEST_CASE("PS3 guitar solo frets, strum, tilt and menu buttons") {
    Gamepad360 g = Translate(Ps3Instrument::kGuitar, Report().Buttons(kTriangle | kBit6));
    CHECK(g.buttons == (kY | kLeftThumb));

    g = Translate(Ps3Instrument::kGuitar, Report().Hat(kHatUp));
    CHECK(g.buttons == kDpadUp);
    g = Translate(Ps3Instrument::kGuitar, Report().Hat(kHatDown));
    CHECK(g.buttons == kDpadDown);
    g = Translate(Ps3Instrument::kGuitar, Report().Hat(kHatUpRight));
    CHECK(g.buttons == (kDpadUp | kDpadRight));
    g = Translate(Ps3Instrument::kGuitar, Report().Hat(kHatCentered));
    CHECK(g.buttons == 0);

    g = Translate(Ps3Instrument::kGuitar, Report().Buttons(kBit5));
    CHECK(g.thumb_ry == 32767);

    g = Translate(Ps3Instrument::kGuitar, Report().Buttons(kStartPs3 | kSelect));
    CHECK(g.buttons == (kStart | kBack));
}

TEST_CASE("PS3 guitar whammy and pickup hold their value at rest") {
    Ps3InstrumentTranslator t(Ps3Instrument::kGuitar);

    auto g = t.Translate(Report().Byte(5, 0x00).Byte(6, 0x4C).bytes);
    REQUIRE(g);
    CHECK(u16(g->thumb_rx) == 0x8000);  // whammy released
    CHECK(g->left_trigger == 0x4C);     // pickup, second notch

    g = t.Translate(Report().Byte(5, 0xFF).Byte(6, 0xE5).bytes);
    REQUIRE(g);
    CHECK(g->thumb_rx == 32767);
    CHECK(g->left_trigger == 0xE5);

    // 0x7F after a moment at rest means unchanged
    g = t.Translate(Report().Byte(5, 0x7F).Byte(6, 0x7F).bytes);
    REQUIRE(g);
    CHECK(g->thumb_rx == 32767);
    CHECK(g->left_trigger == 0xE5);
}

TEST_CASE("reports with a leading report ID, and short reports") {
    Report r;
    r.Buttons(kCross);
    std::vector<uint8_t> with_id(r.bytes.begin(), r.bytes.end());
    with_id.insert(with_id.begin(), 0x00);

    Ps3InstrumentTranslator t(Ps3Instrument::kGuitar);
    auto g = t.Translate(with_id);
    REQUIRE(g);
    CHECK(g->buttons == kA);

    std::vector<uint8_t> short_report(r.bytes.begin(), r.bytes.begin() + 20);
    CHECK_FALSE(t.Translate(short_report).has_value());
}

TEST_CASE("PS3 drum pads land on RB3's slots with the pad flag") {
    const std::pair<uint16_t, int> pads[] = {
        {kCircle, 1}, {kTriangle, 2}, {kSquare, 3}, {kCross, 4}};
    for (const auto& [bit, slot] : pads) {
        const Gamepad360 g = Translate(Ps3Instrument::kDrums, Report().Buttons(bit | kL3));
        CAPTURE(slot);
        CHECK(DrumSlot(JoypadButtonFor(SlotButtons(g.buttons))) == slot);
        CHECK((g.buttons & kRightThumb) != 0);
        CHECK((g.buttons & kRightShoulder) == 0);
    }
}

TEST_CASE("PS3 cymbals and kicks pass across as the 360 flags") {
    Gamepad360 g =
        Translate(Ps3Instrument::kDrums, Report().Buttons(kTriangle | kR3).Hat(kHatUp));
    CHECK(g.buttons == (kY | kRightShoulder | kDpadUp));
    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kSquare | kR3).Hat(kHatDown));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kCross | kR3));
    CHECK(g.buttons == (kA | kRightShoulder));

    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kBit4));
    CHECK(DrumSlot(JoypadButtonFor(g.buttons)) == 0);  // kick
    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kBit5));
    CHECK(g.buttons == kLeftThumb);  // second kick
}

TEST_CASE("PS3 drum velocities move to the 360 velocity axes") {
    // red byte 12 -> LX, yellow 11 -> LY, blue 14 -> RX, green 13 -> RY
    Gamepad360 g = Translate(Ps3Instrument::kDrums,
                             Report().Buttons(kCircle | kL3).Byte(12, 0x00));
    CHECK(g.thumb_lx == 0);  // hardest hit

    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kTriangle | kL3).Byte(11, 0xFF));
    CHECK(u16(g.thumb_ly) == 0xFFFF);  // softest, with yellow's top bit

    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kCross | kL3).Byte(13, 0x80));
    CHECK(u16(g.thumb_ry) == (0x8000 | 0x80 * 0x7FFF / 0xFF));

    g = Translate(Ps3Instrument::kDrums, Report().Buttons(kSquare | kL3).Byte(14, 0x40));
    CHECK(u16(g.thumb_rx) == 0x40 * 0x7FFF / 0xFF);

    // nothing hit, nothing on the axes
    g = Translate(Ps3Instrument::kDrums, Report());
    CHECK(g.thumb_lx == 0);
    CHECK(g.thumb_ly == 0);
    CHECK(g.thumb_rx == 0);
    CHECK(g.thumb_ry == 0);
}

TEST_CASE("RB1 kits send no velocity") {
    const Gamepad360 g =
        Translate(Ps3Instrument::kDrumsRb1, Report().Buttons(kCircle).Byte(12, 0x55));
    CHECK(g.buttons == kB);
    CHECK(g.thumb_lx == 0);
}

TEST_CASE("PS3 instruments report as the matching 360 models") {
    const Caps360 guitar = Ps3InstrumentCaps(Ps3Instrument::kGuitar);
    CHECK(guitar.sub_type == kSubtypeGuitar);
    CHECK_FALSE(SeesRb2Guitar(guitar));  // no auto-calibration sensors
    CHECK_FALSE(SeesMidiProAdapter(guitar));

    const Caps360 drums = Ps3InstrumentCaps(Ps3Instrument::kDrums);
    CHECK(drums.sub_type == kSubtypeDrums);
    CHECK(SeesRb2Drums(drums));
    CHECK_FALSE(SeesMidiProAdapter(drums));

    const Caps360 rb1 = Ps3InstrumentCaps(Ps3Instrument::kDrumsRb1);
    CHECK(rb1.sub_type == kSubtypeDrums);
    CHECK_FALSE(SeesRb2Drums(rb1));
}
