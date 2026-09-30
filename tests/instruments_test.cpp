// Checks the Xbox 360 instrument encoding (src/Input/instruments.cpp) against
// what RB3 does with it (see rb3_reference.h) and PlasticBand's Xbox 360 bit
// layouts.

#include <doctest/doctest.h>
#include <cstdint>
#include "rb3_reference.h"
#include "src/Input/instruments.h"

using namespace band3::input;
using namespace rb3;

TEST_CASE("RB3 accepts only instrument subtypes") {
    for (uint8_t subtype : {6, 7, 8, 9, 11, 15, 25}) {
        CHECK(IsRb3InstrumentSubtype(subtype));
    }
    for (uint8_t subtype : {0, 1, 2, 3, 5, 19}) {
        CHECK_FALSE(IsRb3InstrumentSubtype(subtype));
    }
}

TEST_CASE("guitar frets land on RB3's slots") {
    for (int fret = 0; fret < kFretCount; fret++) {
        GuitarInputs in;
        in.frets[fret] = true;
        const uint16_t button = SlotButtons(EncodeGuitar(in).buttons);
        CAPTURE(fret);
        CHECK(GuitarSlot(JoypadButtonFor(button)) == fret);
    }
}

TEST_CASE("guitar solo frets add RB3's shift button") {
    GuitarInputs in;
    in.frets[kYellow] = true;
    in.solo = true;
    // (shift_button kPad_L3)
    CHECK(EncodeGuitar(in).buttons == (kY | kLeftThumb));

    in.frets = {};
    CHECK((EncodeGuitar(in).buttons & kLeftThumb) == 0);
}

TEST_CASE("guitar strum, whammy, tilt and pickup") {
    GuitarInputs in;
    in.strum_up = true;
    CHECK(EncodeGuitar(in).buttons == kDpadUp);
    in = {};
    in.strum_down = true;
    CHECK(EncodeGuitar(in).buttons == kDpadDown);

    in = {};
    CHECK(u16(EncodeGuitar(in).thumb_rx) == 0x8000);  // released
    in.whammy = 1.0f;
    CHECK(EncodeGuitar(in).thumb_rx == 32767);
    in.tilt = 1.0f;
    CHECK(EncodeGuitar(in).thumb_ry == 32767);
    in.pickup = 0x40;
    CHECK(EncodeGuitar(in).left_trigger == 0x40);
}

TEST_CASE("guitar capabilities read as an RB2 guitar, or RB1-style") {
    const Caps360 rb2 = GuitarCaps();
    CHECK(rb2.sub_type == kSubtypeGuitar);
    CHECK(SeesRb2Guitar(rb2));
    CHECK_FALSE(SeesMidiProAdapter(rb2));

    const Caps360 rb1 = GuitarCaps(false);
    CHECK(rb1.sub_type == kSubtypeGuitar);
    CHECK_FALSE(SeesRb2Guitar(rb1));
    CHECK_FALSE(SeesMidiProAdapter(rb1));
}

TEST_CASE("drum pads land on RB3's slots") {
    for (int pad = 0; pad < kPadCount; pad++) {
        DrumInputs in;
        in.pads[pad] = 100;
        const Gamepad360 g = EncodeDrums(in);
        CAPTURE(pad);
        // red pad is slot 1 through green pad slot 4; slot 0 is the kick
        CHECK(DrumSlot(JoypadButtonFor(SlotButtons(g.buttons))) == pad + 1);
        CHECK((g.buttons & kRightThumb) != 0);     // pad flag
        CHECK((g.buttons & kRightShoulder) == 0);  // no cymbal flag
    }

    DrumInputs kick;
    kick.kick1 = true;
    CHECK(DrumSlot(JoypadButtonFor(EncodeDrums(kick).buttons)) == 0);
    kick = {};
    kick.kick2 = true;
    CHECK(EncodeDrums(kick).buttons == kLeftThumb);
}

TEST_CASE("cymbals set the cymbal flag and their d-pad marker") {
    DrumInputs in;
    in.cymbals[kYellowCymbal] = 100;
    CHECK(EncodeDrums(in).buttons == (kY | kRightShoulder | kDpadUp));
    in = {};
    in.cymbals[kBlueCymbal] = 100;
    CHECK(EncodeDrums(in).buttons == (kX | kRightShoulder | kDpadDown));
    in = {};
    in.cymbals[kGreenCymbal] = 100;
    CHECK(EncodeDrums(in).buttons == (kA | kRightShoulder));
}

TEST_CASE("drum velocities use RB3's velocity axes, inverted") {
    // hx_drums_xbox_rb2 (velocity_axes (1 LX) (2 LY) (3 RX) (4 RY))
    DrumInputs in;
    in.pads[kRedPad] = 127;
    CHECK(EncodeDrums(in).thumb_lx == 0);  // hardest hit
    in = {};
    in.pads[kYellowPad] = 1;
    CHECK(u16(EncodeDrums(in).thumb_ly) == 0xFFFF);  // softest, with the top bit
    in = {};
    in.pads[kBluePad] = 1;
    CHECK(u16(EncodeDrums(in).thumb_rx) == 0x7FFF);
    in = {};
    in.pads[kGreenPad] = 127;
    CHECK(u16(EncodeDrums(in).thumb_ry) == 0x8000);
}

TEST_CASE("a pad and cymbal of one color put the cymbal's velocity in red") {
    DrumInputs in;
    in.pads[kBluePad] = 127;
    in.cymbals[kBlueCymbal] = 1;
    const Gamepad360 g = EncodeDrums(in);
    CHECK(g.buttons == (kX | kRightThumb | kRightShoulder | kDpadDown));
    CHECK(g.thumb_rx == 0);                 // the pad's velocity
    CHECK(u16(g.thumb_lx) == 0x7FFF);       // the cymbal's, without the red button
}

TEST_CASE("drum capabilities read as RB2 drums, or an RB1 kit") {
    const Caps360 rb2 = DrumCaps();
    CHECK(rb2.sub_type == kSubtypeDrums);
    CHECK(SeesRb2Drums(rb2));
    CHECK_FALSE(SeesMidiProAdapter(rb2));

    // RB1 kits: no force feedback flag and all-zero trigger and stick capabilities
    const Caps360 rb1 = DrumCaps(false);
    CHECK(rb1.sub_type == kSubtypeDrums);
    CHECK_FALSE(SeesRb2Drums(rb1));
    CHECK(rb1.flags == 0);
    CHECK(rb1.gamepad.left_trigger == 0);
    CHECK(rb1.gamepad.right_trigger == 0);
    CHECK(rb1.gamepad.thumb_lx == 0);
    CHECK(rb1.gamepad.thumb_rx == 0);
}

TEST_CASE("keys pack into the triggers and left stick") {
    KeysInputs in;
    in.keys[0] = 100;  // C1
    Gamepad360 g = EncodeKeys(in);
    CHECK(g.left_trigger == 0x80);
    CHECK(u16(g.thumb_lx) == (100 << 8));  // first velocity

    in = {};
    in.keys[7] = 1;   // G1
    in.keys[8] = 1;   // Ab1
    in.keys[15] = 1;  // Eb2
    g = EncodeKeys(in);
    CHECK(g.left_trigger == 0x01);
    CHECK(g.right_trigger == 0x81);

    in = {};
    in.keys[16] = 64;  // E2
    in.keys[24] = 64;  // C3
    g = EncodeKeys(in);
    CHECK(u16(g.thumb_lx) == (0x8000 | (64 << 8) | 0x0080));
    CHECK(u16(g.thumb_ly) == 64);  // second velocity
}

TEST_CASE("keys pair up to five velocities with the lowest held keys") {
    KeysInputs in;
    for (int k = 0; k < 6; k++) in.keys[k] = static_cast<uint8_t>(10 + k);
    const Gamepad360 g = EncodeKeys(in);
    CHECK(((u16(g.thumb_lx) >> 8) & 0x7F) == 10);
    CHECK((u16(g.thumb_ly) & 0x7F) == 11);
    CHECK(((u16(g.thumb_ly) >> 8) & 0x7F) == 12);
    CHECK((u16(g.thumb_rx) & 0x7F) == 13);
    CHECK(((u16(g.thumb_rx) >> 8) & 0x7F) == 14);
}

TEST_CASE("keys overdrive and an empty pedal port") {
    KeysInputs in;
    CHECK(u16(EncodeKeys(in).thumb_ry) == 0x7F00);
    in.overdrive = true;
    CHECK(u16(EncodeKeys(in).thumb_ry) == 0x7F80);
}

TEST_CASE("keys capabilities read as a keytar, not a MIDI Pro Adapter") {
    const Caps360 c = KeysCaps();
    CHECK(c.sub_type == kSubtypeKeytar);
    CHECK(SeesKeytar(c));
}

TEST_CASE("pro guitar frets, velocities and color flags") {
    ProGuitarInputs in;
    in.frets[kStringLowE] = 5;
    in.frets[kStringA] = 3;
    in.frets[kStringD] = 22;
    in.frets[kStringHighE] = 12;
    in.velocities[kStringLowE] = 100;
    in.velocities[kStringHighE] = 7;
    in.colors[kGreen] = true;
    in.colors[kBlue] = true;
    in.colors[kOrange] = true;
    in.solo = true;
    const Gamepad360 g = EncodeProGuitar(in);

    const unsigned triggers = g.left_trigger | (g.right_trigger << 8);
    CHECK(triggers == (5u | 3u << 5 | 22u << 10));
    CHECK(u16(g.thumb_lx) == (12 << 10 | 0x8000));
    CHECK(u16(g.thumb_ly) == (100 | 0x80));
    CHECK(u16(g.thumb_rx) == 0x8000);
    CHECK(u16(g.thumb_ry) == (0x80 | 7 << 8));
}

TEST_CASE("pro guitar frets clamp to 22") {
    ProGuitarInputs in;
    in.frets[kStringLowE] = 40;
    const Gamepad360 g = EncodeProGuitar(in);
    CHECK((g.left_trigger & 0x1F) == 22);
}

TEST_CASE("pro guitar capabilities pick the model") {
    CHECK(ProGuitarCaps(ProGuitarModel::kSquier).sub_type == kSubtypeProGuitar);
    CHECK((u16(ProGuitarCaps(ProGuitarModel::kSquier).gamepad.thumb_ly) & 0xFFF0) == 0x1530);
    CHECK((u16(ProGuitarCaps(ProGuitarModel::kMustang).gamepad.thumb_ly) & 0xFFF0) == 0x1430);
}

TEST_CASE("pro data is the report from the left trigger on, little-endian") {
    Gamepad360 g;
    g.left_trigger = 0x11;
    g.right_trigger = 0x22;
    g.thumb_lx = static_cast<int16_t>(0x4433);
    g.thumb_ly = static_cast<int16_t>(0x6655);
    g.thumb_rx = static_cast<int16_t>(0x8877);
    g.thumb_ry = static_cast<int16_t>(0xAA99);
    const ProData d = EncodeProData(g);
    const ProData expected = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88,
                              0x99, 0xAA, 0, 0, 0, 0, 0, 0};
    CHECK(d == expected);
}

TEST_CASE("Pro Keys reach RB3's keyboard poll") {
    KeysInputs in;
    in.keys[0] = 100;   // C1
    in.keys[7] = 50;
    in.keys[12] = 90;
    in.keys[24] = 127;  // C3
    in.overdrive = true;
    const ProKeysRead r = ReadProKeys(EncodeProData(EncodeKeys(in)));

    for (int k = 0; k < kKeyCount; k++) {
        CAPTURE(k);
        CHECK(r.pressed[k] == (in.keys[k] != 0));
        CHECK(r.velocity[k] == in.keys[k]);
    }
    // the overdrive button is the sustain bit; an empty pedal port reads as the
    // pedal fully back, not stomped
    CHECK(r.sustain);
    CHECK_FALSE(r.stomp_pedal);
    CHECK(r.expression_pedal == 127);
}

TEST_CASE("Pro Keys past five held keys have no velocity slot") {
    KeysInputs in;
    for (int k = 0; k < 6; k++) in.keys[k] = static_cast<uint8_t>(10 + k);
    const ProKeysRead r = ReadProKeys(EncodeProData(EncodeKeys(in)));
    for (int k = 0; k < 5; k++) CHECK(r.velocity[k] == 10 + k);
    CHECK(r.pressed[5]);
    CHECK(r.velocity[5] == 0);
    CHECK_FALSE(r.sustain);
}

TEST_CASE("Pro Guitar reaches RB3's guitar poll") {
    ProGuitarInputs in;
    in.frets = {3, 17, 22, 0, 12, 21};         // low E to high E
    in.velocities = {10, 20, 30, 40, 50, 127};
    in.colors[kGreen] = true;
    in.colors[kYellow] = true;
    in.colors[kOrange] = true;
    in.solo = true;
    const ProGuitarRead r = ReadProGuitar(EncodeProData(EncodeProGuitar(in)));

    // the game numbers strings from high E (0) to low E (5)
    for (int s = 0; s < kStringCount; s++) {
        CAPTURE(s);
        CHECK(r.fret[5 - s] == in.frets[s]);
        CHECK(r.velocity[5 - s] == in.velocities[s]);
    }
    for (int f = 0; f < kFretCount; f++) {
        CAPTURE(f);
        CHECK(r.fret_down[f] == in.colors[f]);
    }
    CHECK(r.solo);
}

TEST_CASE("an idle Pro Guitar reads as open, silent strings") {
    const ProGuitarRead r = ReadProGuitar(EncodeProData(EncodeProGuitar({})));
    for (int j = 0; j < 6; j++) {
        CHECK(r.fret[j] == 0);
        CHECK(r.velocity[j] == 0);
    }
    CHECK_FALSE(r.solo);
}
