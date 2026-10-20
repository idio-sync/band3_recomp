// Checks the PS3/Wii instrument translation (src/Input/ps3_instruments.cpp).
// Reports are built from PlasticBand's documented layout (Docs/Base Reports/PS3.md
// and the PS3 and Wii guitar and drum docs), and the output is checked against
// what RB3 does with an Xbox 360 instrument (rb3_reference.h).

#include <doctest/doctest.h>
#include <algorithm>
#include <array>
#include <cstdint>
#include <vector>
#include "rb3_reference.h"
#include "src/Input/hid_instrument_types.h"

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

Gamepad360 Translate(HidInstrumentType instrument, const Report& report) {
    HidInstrumentTranslator t(instrument);
    auto g = t.Translate(report.bytes);
    REQUIRE(g.has_value());
    return *g;
}

}

TEST_CASE("PS3 and Wii instruments are identified by USB id") {
    CHECK(IdentifyHidInstrument(0x12BA, 0x0200, 0x0313) == HidInstrumentType::kPs3Guitar);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x0004, 0x0100) == HidInstrumentType::kPs3Guitar);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3010, 0x0200) == HidInstrumentType::kPs3Guitar);

    // RB1 PS3 kits report release 0x1000, RB2 ones 0x0200
    CHECK(IdentifyHidInstrument(0x12BA, 0x0210, 0x1000) == HidInstrumentType::kPs3DrumsRb1);
    CHECK(IdentifyHidInstrument(0x12BA, 0x0210, 0x0200) == HidInstrumentType::kPs3Drums);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x0005, 0x0100) == HidInstrumentType::kPs3DrumsRb1);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3110, 0x0200) == HidInstrumentType::kPs3Drums);

    // MIDI Pro Adapter in drum mode
    CHECK(IdentifyHidInstrument(0x12BA, 0x0218, 0) == HidInstrumentType::kPs3Drums);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3138, 0) == HidInstrumentType::kPs3Drums);

    // Guitar Hero guitar, DualShock 3
    CHECK_FALSE(IdentifyHidInstrument(0x12BA, 0x0100, 0).has_value());
    CHECK_FALSE(IdentifyHidInstrument(0x054C, 0x0268, 0).has_value());

    for (const auto& known : KnownHidInstruments()) {
        CAPTURE(known.name);
        CHECK(IdentifyHidInstrument(known.vendor, known.product, 0).has_value());
    }
}

TEST_CASE("PS3 guitar frets land on RB3's slots") {
    const std::pair<uint16_t, int> frets[] = {
        {kCross, 0}, {kCircle, 1}, {kTriangle, 2}, {kSquare, 3}, {kBit4, 4}};
    for (const auto& [bit, slot] : frets) {
        const Gamepad360 g = Translate(HidInstrumentType::kPs3Guitar, Report().Buttons(bit));
        CAPTURE(slot);
        CHECK(GuitarSlot(JoypadButtonFor(SlotButtons(g.buttons))) == slot);
    }
}

TEST_CASE("PS3 guitar solo frets, strum, tilt and menu buttons") {
    Gamepad360 g = Translate(HidInstrumentType::kPs3Guitar, Report().Buttons(kTriangle | kBit6));
    CHECK(g.buttons == (kY | kLeftThumb));

    g = Translate(HidInstrumentType::kPs3Guitar, Report().Hat(kHatUp));
    CHECK(g.buttons == kDpadUp);
    g = Translate(HidInstrumentType::kPs3Guitar, Report().Hat(kHatDown));
    CHECK(g.buttons == kDpadDown);
    g = Translate(HidInstrumentType::kPs3Guitar, Report().Hat(kHatUpRight));
    CHECK(g.buttons == (kDpadUp | kDpadRight));
    g = Translate(HidInstrumentType::kPs3Guitar, Report().Hat(kHatCentered));
    CHECK(g.buttons == 0);

    g = Translate(HidInstrumentType::kPs3Guitar, Report().Buttons(kBit5));
    CHECK(g.thumb_ry == 32767);

    g = Translate(HidInstrumentType::kPs3Guitar, Report().Buttons(kStartPs3 | kSelect));
    CHECK(g.buttons == (kStart | kBack));
}

TEST_CASE("PS3 guitar whammy and pickup hold their value at rest") {
    HidInstrumentTranslator t(HidInstrumentType::kPs3Guitar);

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

    HidInstrumentTranslator t(HidInstrumentType::kPs3Guitar);
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
        const Gamepad360 g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(bit | kL3));
        CAPTURE(slot);
        CHECK(DrumSlot(JoypadButtonFor(SlotButtons(g.buttons))) == slot);
        CHECK((g.buttons & kRightThumb) != 0);
        CHECK((g.buttons & kRightShoulder) == 0);
    }
}

TEST_CASE("PS3 cymbals and kicks pass across as the 360 flags") {
    Gamepad360 g =
        Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kTriangle | kR3).Hat(kHatUp));
    CHECK(g.buttons == (kY | kRightShoulder | kDpadUp));
    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kSquare | kR3).Hat(kHatDown));
    CHECK(g.buttons == (kX | kRightShoulder | kDpadDown));
    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kCross | kR3));
    CHECK(g.buttons == (kA | kRightShoulder));

    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kBit4));
    CHECK(DrumSlot(JoypadButtonFor(g.buttons)) == 0);  // kick
    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kBit5));
    CHECK(g.buttons == kLeftThumb);  // second kick
}

TEST_CASE("PS3 drum velocities move to the 360 velocity axes") {
    // red byte 12 -> LX, yellow 11 -> LY, blue 14 -> RX, green 13 -> RY
    Gamepad360 g = Translate(HidInstrumentType::kPs3Drums,
                             Report().Buttons(kCircle | kL3).Byte(12, 0x00));
    CHECK(g.thumb_lx == 0);  // hardest hit

    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kTriangle | kL3).Byte(11, 0xFF));
    CHECK(u16(g.thumb_ly) == 0xFFFF);  // softest, with yellow's top bit

    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kCross | kL3).Byte(13, 0x80));
    CHECK(u16(g.thumb_ry) == (0x8000 | 0x80 * 0x7FFF / 0xFF));

    g = Translate(HidInstrumentType::kPs3Drums, Report().Buttons(kSquare | kL3).Byte(14, 0x40));
    CHECK(u16(g.thumb_rx) == 0x40 * 0x7FFF / 0xFF);

    // nothing hit, nothing on the axes
    g = Translate(HidInstrumentType::kPs3Drums, Report());
    CHECK(g.thumb_lx == 0);
    CHECK(g.thumb_ly == 0);
    CHECK(g.thumb_rx == 0);
    CHECK(g.thumb_ry == 0);
}

TEST_CASE("RB1 kits send no velocity") {
    const Gamepad360 g =
        Translate(HidInstrumentType::kPs3DrumsRb1, Report().Buttons(kCircle).Byte(12, 0x55));
    CHECK(g.buttons == kB);
    CHECK(g.thumb_lx == 0);
}

TEST_CASE("PS3 instruments report as the matching 360 models") {
    const Caps360 guitar = HidInstrumentCaps(HidInstrumentType::kPs3Guitar);
    CHECK(guitar.sub_type == kSubtypeGuitar);
    CHECK_FALSE(SeesRb2Guitar(guitar));  // no auto-calibration sensors
    CHECK_FALSE(SeesMidiProAdapter(guitar));

    const Caps360 drums = HidInstrumentCaps(HidInstrumentType::kPs3Drums);
    CHECK(drums.sub_type == kSubtypeDrums);
    CHECK(SeesRb2Drums(drums));
    CHECK_FALSE(SeesMidiProAdapter(drums));

    const Caps360 rb1 = HidInstrumentCaps(HidInstrumentType::kPs3DrumsRb1);
    CHECK(rb1.sub_type == kSubtypeDrums);
    CHECK_FALSE(SeesRb2Drums(rb1));
}

TEST_CASE("PS3 and Wii RB3 instruments are identified by USB id") {
    CHECK(IdentifyHidInstrument(0x12BA, 0x2330, 0x0005) == HidInstrumentType::kPs3Keys);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3330, 0x0005) == HidInstrumentType::kPs3Keys);
    CHECK(IdentifyHidInstrument(0x12BA, 0x2338, 0x0001) == HidInstrumentType::kPs3Keys);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3338, 0) == HidInstrumentType::kPs3Keys);
    CHECK(IdentifyHidInstrument(0x12BA, 0x2430, 0) == HidInstrumentType::kPs3Mustang);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3430, 0x0005) == HidInstrumentType::kPs3Mustang);
    CHECK(IdentifyHidInstrument(0x12BA, 0x2438, 0) == HidInstrumentType::kPs3Mustang);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3438, 0) == HidInstrumentType::kPs3Mustang);
    CHECK(IdentifyHidInstrument(0x12BA, 0x2538, 0x0001) == HidInstrumentType::kPs3Squier);
    CHECK(IdentifyHidInstrument(0x1BAD, 0x3538, 0) == HidInstrumentType::kPs3Squier);

    // only the PS3 ones need asking for their keys and frets
    CHECK(NeedsPs3ProEnable(0x12BA, HidInstrumentType::kPs3Keys));
    CHECK(NeedsPs3ProEnable(0x12BA, HidInstrumentType::kPs3Mustang));
    CHECK(NeedsPs3ProEnable(0x12BA, HidInstrumentType::kPs3Squier));
    CHECK_FALSE(NeedsPs3ProEnable(0x1BAD, HidInstrumentType::kPs3Keys));
    CHECK_FALSE(NeedsPs3ProEnable(0x12BA, HidInstrumentType::kPs3Guitar));
}

TEST_CASE("PS3 keyboard keys and velocities reach RB3's keyboard poll") {
    // C1, B2 and C3 held; velocities for the first three held keys, overdrive,
    // the stomp pedal down
    const Report report = Report()
                              .Buttons(kCross)
                              .Hat(kHatUp)
                              .Byte(5, 0x80)
                              .Byte(6, 0x00)
                              .Byte(7, 0x01)
                              .Byte(8, 0x80 | 100)
                              .Byte(9, 90)
                              .Byte(10, 80)
                              .Byte(13, 0x80)
                              .Byte(14, 0xFF);
    const Gamepad360 g = Translate(HidInstrumentType::kPs3Keys, report);
    CHECK(g.buttons == (kA | kDpadUp));

    const ProKeysRead read = ReadProKeys(EncodeProData(g));
    for (int k = 0; k < kKeyCount; k++) {
        CAPTURE(k);
        CHECK(read.pressed[k] == (k == 0 || k == 23 || k == 24));
    }
    CHECK(read.velocity[0] == 100);
    CHECK(read.velocity[23] == 90);
    CHECK(read.velocity[24] == 80);
    CHECK(read.stomp_pedal);
    CHECK(read.expression_pedal == 0x7F);

    // and it's what band3's own keytar encoding gives for the same keys
    const KeysInputs keys = DecodeKeys(g);
    CHECK(keys.overdrive);
    CHECK(keys.keys[0] == 100);
    CHECK(keys.keys[23] == 90);
    CHECK(keys.keys[24] == 80);
}

TEST_CASE("PS3 Pro Guitar frets, strings and colors reach RB3's guitar poll") {
    // low E 3, A 5, D 7, G 12, B open, high E 22, solo; low E, A and high E
    // sounding; green and blue flags
    const Report report = Report()
                              .Buttons(kCircle | kStartPs3)
                              .Byte(5, 0xA3)
                              .Byte(6, 0x1C)
                              .Byte(7, 0x0C)
                              .Byte(8, 0xD8)
                              .Byte(9, 0x80 | 64)
                              .Byte(10, 50)
                              .Byte(11, 0)
                              .Byte(12, 0x80)
                              .Byte(13, 0)
                              .Byte(14, 100);
    const Gamepad360 g = Translate(HidInstrumentType::kPs3Mustang, report);
    CHECK(g.buttons == (kB | kStart));

    const ProGuitarRead read = ReadProGuitar(EncodeProData(g));
    CHECK(read.fret == std::array<int, 6>{22, 0, 12, 7, 5, 3});
    CHECK(read.velocity == std::array<int, 6>{100, 0, 0, 0, 50, 64});
    CHECK(read.fret_down == std::array<bool, 5>{true, false, false, true, false});
    CHECK(read.solo);

    ProGuitarInputs in;
    in.frets = {3, 5, 7, 12, 0, 22};
    in.velocities = {64, 50, 0, 0, 0, 100};
    in.colors = {true, false, false, true, false};
    in.solo = true;
    Gamepad360 encoded = EncodeProGuitar(in);
    encoded.buttons = g.buttons;
    CHECK(g == encoded);
}

TEST_CASE("PS3 RB3 instruments report as the matching 360 models") {
    const Caps360 keys = HidInstrumentCaps(HidInstrumentType::kPs3Keys);
    CHECK(keys.sub_type == kSubtypeKeytar);
    CHECK(SeesKeytar(keys));
    CHECK(HidInstrumentCaps(HidInstrumentType::kPs3Mustang) ==
          ProGuitarCaps(ProGuitarModel::kMustang));
    CHECK(HidInstrumentCaps(HidInstrumentType::kPs3Squier) ==
          ProGuitarCaps(ProGuitarModel::kSquier));
}

TEST_CASE("the PS3 enable report is PlasticBand's 40 bytes in five reports") {
    const uint8_t expected[40] = {
        0xE9, 0x00, 0x89, 0x1B, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00, 0x00, 0x89, 0x00,
        0x00, 0x00, 0x00, 0x00, 0xE9, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    std::vector<uint8_t> joined;
    for (const auto& report : Ps3ProEnableReports()) {
        CHECK(report[0] == 0);  // report ID
        joined.insert(joined.end(), report.begin() + 1, report.end());
    }
    CHECK(joined == std::vector<uint8_t>(std::begin(expected), std::end(expected)));
}

namespace {

// what SendPs3ProEnable sent, and which sizes the device took
struct EnableLog {
    std::vector<std::vector<uint8_t>> sent;
    size_t accepts_up_to = 64;
    bool Send(std::span<const uint8_t> report) {
        sent.emplace_back(report.begin(), report.end());
        return report.size() <= accepts_up_to;
    }
};

}

TEST_CASE("the PS3 enable goes as one 40-byte report, as RB3 sends it") {
    // RB3 enables keys with one wLength 40 SET_REPORT; a device takes 8-byte
    // ones too (RB3's LED commands), so they mustn't be tried first
    EnableLog log;
    CHECK(SendPs3ProEnable([&](std::span<const uint8_t> r) { return log.Send(r); }) ==
          Ps3EnableResult::kWhole);
    REQUIRE(log.sent.size() == 1);
    REQUIRE(log.sent[0].size() == 41);
    CHECK(log.sent[0][0] == 0);     // report ID
    CHECK(log.sent[0][3] == 0x89);  // what RPCS3 reads as "MIDI data enabled"
}

TEST_CASE("the PS3 enable falls back to five reports if the long one isn't taken") {
    EnableLog log;
    log.accepts_up_to = 9;
    CHECK(SendPs3ProEnable([&](std::span<const uint8_t> r) { return log.Send(r); }) ==
          Ps3EnableResult::kSplit);
    REQUIRE(log.sent.size() == 6);
    for (size_t i = 1; i < log.sent.size(); i++) CHECK(log.sent[i].size() == 9);

    EnableLog none;
    none.accepts_up_to = 0;
    CHECK(SendPs3ProEnable([&](std::span<const uint8_t> r) { return none.Send(r); }) ==
          Ps3EnableResult::kFailed);
}

TEST_CASE("the PS3 keyboard's touchpad reaches RB3 as the keys' whammy") {
    HidInstrumentTranslator t(HidInstrumentType::kPs3Keys);
    // the touchpad in byte 15's low 7 bits, the top bit not part of it
    const Report report = Report().Byte(5, 0x80).Byte(8, 100).Byte(15, 0x80 | 0x55);
    const auto g = t.Translate(report.bytes);
    REQUIRE(g.has_value());

    ProData data = EncodeProData(*g);
    std::copy(t.pro_extra().begin(), t.pro_extra().end(), data.begin() + kProExtraOffset);
    const ProKeysRead read = ReadProKeys(data);
    CHECK(read.mod == 0x55);
    // and it leaves the keys alone
    CHECK(read.pressed[0]);
    CHECK(read.velocity[0] == 100);

    // let go, it's back to 0
    REQUIRE(t.Translate(Report().bytes).has_value());
    CHECK(t.pro_extra() == ProExtra{});
}

TEST_CASE("only the keyboard sends pro data past the XInput state") {
    for (const auto type : {HidInstrumentType::kPs3Mustang, HidInstrumentType::kPs3Squier,
                            HidInstrumentType::kPs3Guitar}) {
        HidInstrumentTranslator t(type);
        REQUIRE(t.Translate(Report().Byte(15, 0x7F).bytes).has_value());
        CHECK(t.pro_extra() == ProExtra{});
    }
}

TEST_CASE("a short PS3 keyboard report is dropped") {
    HidInstrumentTranslator t(HidInstrumentType::kPs3Keys);
    const std::array<uint8_t, 10> short_report{};
    CHECK_FALSE(t.Translate(short_report).has_value());
}
