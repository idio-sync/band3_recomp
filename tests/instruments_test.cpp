// Checks the Xbox 360 instrument encoding (src/Input/instruments.cpp) against
// what RB3 does with it:
// - fret and pad slots from RB3's config/beatmatch_controller.dta (the copy in
//   Rock Band 3 Deluxe's repo, _ark/config/beatmatch_controller.dta)
// - model detection from ReadSingleXinputJoypad and the SetupHX* functions in
//   the rb3-xenon decomp (src/system/os/Joypad_Xinput.cpp, Joypad_Xbox.cpp)
// - bit layouts from PlasticBand's Xbox 360 docs

#include <doctest/doctest.h>
#include <cstdint>
#include "src/Input/instruments.h"

using namespace band3::input;

namespace {

constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kLeftThumb = 0x0040;
constexpr uint16_t kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100;
constexpr uint16_t kRightShoulder = 0x0200;
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kX = 0x4000;
constexpr uint16_t kY = 0x8000;

uint16_t u16(int16_t v) { return static_cast<uint16_t>(v); }

// RB3's JoypadButton numbers for the Xbox buttons (kPad_Xbox_* in Joypad.h),
// which beatmatch_controller.dta's slot tables use
int JoypadButtonFor(uint16_t xinput_button) {
    switch (xinput_button) {
    case kLeftShoulder: return 2;
    case kRightShoulder: return 3;
    case kY: return 4;
    case kB: return 5;
    case kA: return 6;
    case kX: return 7;
    case kLeftThumb: return 9;
    case kRightThumb: return 10;
    default: return -1;
    }
}

// (slots 6 0 5 1 4 2 7 3 2 4), shared by every Xbox guitar entry (ro_guitar_xbox,
// real_guitar, ...): JoypadButton -> fret slot, green 0 to orange 4
int GuitarSlot(int joypad_button) {
    constexpr int kSlots[][2] = {{6, 0}, {5, 1}, {4, 2}, {7, 3}, {2, 4}};
    for (const auto& s : kSlots) {
        if (s[0] == joypad_button) return s[1];
    }
    return -1;
}

// hx_drums_xbox_rb2 (slots 2 0 5 1 4 2 7 3 6 4): kick 0, red 1, yellow 2, blue 3, green 4
int DrumSlot(int joypad_button) {
    constexpr int kSlots[][2] = {{2, 0}, {5, 1}, {4, 2}, {7, 3}, {6, 4}};
    for (const auto& s : kSlots) {
        if (s[0] == joypad_button) return s[1];
    }
    return -1;
}

// the one face or shoulder button set in `buttons`, ignoring flags and the d-pad
uint16_t OnlySlotButton(uint16_t buttons) {
    return buttons & (kA | kB | kX | kY | kLeftShoulder);
}

// RB3's model checks, as the decomp has them
bool Rb3SeesRb2Guitar(const Caps360& c) {
    bool wireless = c.flags & 0x2;
    return wireless && ((c.flags & 1) || c.gamepad.thumb_rx >= 0x100) &&
           u16(c.gamepad.thumb_lx) != 0x1BAD;
}
bool Rb3SeesRb2Drums(const Caps360& c) {
    return ((c.flags & 1) || c.gamepad.thumb_rx >= 0x100) && u16(c.gamepad.thumb_lx) != 0x1BAD;
}
bool Rb3SeesKeytar(const Caps360& c) {
    return (u16(c.gamepad.thumb_ly) & 0xFFF0) != 0x1730;  // 0x173x is a MIDI Pro Adapter
}

}

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
        const uint16_t button = OnlySlotButton(EncodeGuitar(in).buttons);
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
    in.tilt = true;
    CHECK(EncodeGuitar(in).thumb_ry == 32767);
    in.pickup = 0x40;
    CHECK(EncodeGuitar(in).left_trigger == 0x40);
}

TEST_CASE("guitar capabilities read as an RB2 guitar") {
    const Caps360 c = GuitarCaps();
    CHECK(c.sub_type == kSubtypeGuitar);
    CHECK(Rb3SeesRb2Guitar(c));
}

TEST_CASE("drum pads land on RB3's slots") {
    for (int pad = 0; pad < kPadCount; pad++) {
        DrumInputs in;
        in.pads[pad] = 100;
        const Gamepad360 g = EncodeDrums(in);
        CAPTURE(pad);
        // red pad is slot 1 through green pad slot 4; slot 0 is the kick
        CHECK(DrumSlot(JoypadButtonFor(OnlySlotButton(g.buttons))) == pad + 1);
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

TEST_CASE("drum capabilities read as RB2 drums") {
    const Caps360 c = DrumCaps();
    CHECK(c.sub_type == kSubtypeDrums);
    CHECK(Rb3SeesRb2Drums(c));
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
    CHECK(Rb3SeesKeytar(c));
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
