// Checks the launcher test view's decoders (src/Input/instruments.cpp):
// encoding an instrument's inputs and decoding them gives the inputs back, for
// every guitar, drum and keytar input, and RB1 kits read without flags or
// velocity.

#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "src/Input/instruments.h"

using namespace band3::input;

namespace {

bool SameNav(const NavInputs& a, const NavInputs& b) {
    return a.a == b.a && a.b == b.b && a.x == b.x && a.y == b.y && a.start == b.start &&
           a.back == b.back && a.dpad_up == b.dpad_up && a.dpad_down == b.dpad_down &&
           a.dpad_left == b.dpad_left && a.dpad_right == b.dpad_right;
}

void CheckDrums(const DrumInputs& in, const DrumInputs& out) {
    for (int p = 0; p < kPadCount; p++) {
        CAPTURE(p);
        CHECK(out.pads[p] == in.pads[p]);
    }
    for (int c = 0; c < kCymbalCount; c++) {
        CAPTURE(c);
        CHECK(out.cymbals[c] == in.cymbals[c]);
    }
    CHECK(out.kick1 == in.kick1);
    CHECK(out.kick2 == in.kick2);
}

// encode, decode, and the decoded inputs encode the same again
DrumInputs RoundTrip(const DrumInputs& in, const Caps360& caps = DrumCaps()) {
    const Gamepad360 g = EncodeDrums(in);
    const DrumInputs out = DecodeDrums(g, caps);
    if (IsRb2Drums(caps)) CHECK(EncodeDrums(out).buttons == g.buttons);
    return out;
}

}

TEST_CASE("each guitar fret decodes back") {
    for (int fret = 0; fret < kFretCount; fret++) {
        CAPTURE(fret);
        GuitarInputs in;
        in.frets[fret] = true;
        const GuitarInputs out = DecodeGuitar(EncodeGuitar(in));
        CHECK(out.frets == in.frets);
        CHECK_FALSE(out.solo);
        CHECK_FALSE(out.strum_up);
        CHECK_FALSE(out.strum_down);
    }
    GuitarInputs all;
    all.frets = {true, true, true, true, true};
    CHECK(DecodeGuitar(EncodeGuitar(all)).frets == all.frets);
}

TEST_CASE("the green fret is A to the menus, as the game reads it") {
    GuitarInputs in;
    in.frets[kGreen] = true;
    const GuitarInputs out = DecodeGuitar(EncodeGuitar(in));
    CHECK(out.nav.a);
    CHECK_FALSE(out.nav.b);
}

TEST_CASE("guitar solo, strums and the menu buttons decode back") {
    GuitarInputs in;
    in.frets[kBlue] = true;
    in.solo = true;
    GuitarInputs out = DecodeGuitar(EncodeGuitar(in));
    CHECK(out.solo);
    CHECK(out.frets == in.frets);

    in = {};
    in.strum_up = true;
    out = DecodeGuitar(EncodeGuitar(in));
    CHECK(out.strum_up);
    CHECK_FALSE(out.strum_down);
    in = {};
    in.strum_down = true;
    out = DecodeGuitar(EncodeGuitar(in));
    CHECK(out.strum_down);
    CHECK_FALSE(out.strum_up);

    in = {};
    in.nav.start = true;
    in.nav.back = true;
    in.nav.dpad_left = true;
    in.nav.dpad_right = true;
    out = DecodeGuitar(EncodeGuitar(in));
    CHECK(SameNav(out.nav, in.nav));
    CHECK(out.frets == std::array<bool, kFretCount>{});
}

TEST_CASE("guitar whammy, tilt and pickup decode back") {
    for (float w : {0.0f, 0.25f, 0.5f, 0.75f, 1.0f}) {
        CAPTURE(w);
        GuitarInputs in;
        in.whammy = w;
        CHECK(DecodeGuitar(EncodeGuitar(in)).whammy == doctest::Approx(w).epsilon(0.0001));
    }
    for (float t : {0.0f, 0.3f, 1.0f}) {
        CAPTURE(t);
        GuitarInputs in;
        in.tilt = t;
        CHECK(DecodeGuitar(EncodeGuitar(in)).tilt == doctest::Approx(t).epsilon(0.0001));
    }
    for (int pickup : {0, 0x19, 0x4C, 0x96, 0xFF}) {
        CAPTURE(pickup);
        GuitarInputs in;
        in.pickup = static_cast<uint8_t>(pickup);
        CHECK(DecodeGuitar(EncodeGuitar(in)).pickup == pickup);
    }
    // a guitar tilted down reads as level
    Gamepad360 g;
    g.thumb_ry = -12000;
    CHECK(DecodeGuitar(g).tilt == 0.0f);
}

TEST_CASE("each drum pad decodes back at every velocity") {
    for (int pad = 0; pad < kPadCount; pad++) {
        for (int velocity = 1; velocity <= 127; velocity++) {
            CAPTURE(pad);
            CAPTURE(velocity);
            DrumInputs in;
            in.pads[pad] = static_cast<uint8_t>(velocity);
            CheckDrums(in, RoundTrip(in));
        }
    }
}

TEST_CASE("each cymbal decodes back at every velocity") {
    for (int cymbal = 0; cymbal < kCymbalCount; cymbal++) {
        for (int velocity = 1; velocity <= 127; velocity++) {
            CAPTURE(cymbal);
            CAPTURE(velocity);
            DrumInputs in;
            in.cymbals[cymbal] = static_cast<uint8_t>(velocity);
            CheckDrums(in, RoundTrip(in));
        }
    }
}

TEST_CASE("kicks decode back") {
    DrumInputs in;
    in.kick1 = true;
    CheckDrums(in, RoundTrip(in));
    in = {};
    in.kick2 = true;
    CheckDrums(in, RoundTrip(in));
    in.kick1 = true;
    in.pads[kRedPad] = 90;
    CheckDrums(in, RoundTrip(in));
}

TEST_CASE("a pad and a cymbal of different colors decode back") {
    struct Hit {
        DrumPad pad;
        Cymbal cymbal;
    };
    const std::vector<Hit> hits = {
        {kRedPad, kYellowCymbal},   {kRedPad, kBlueCymbal},    {kRedPad, kGreenCymbal},
        {kYellowPad, kBlueCymbal},  {kYellowPad, kGreenCymbal}, {kBluePad, kYellowCymbal},
        {kBluePad, kGreenCymbal},   {kGreenPad, kYellowCymbal}, {kGreenPad, kBlueCymbal},
    };
    for (const auto& hit : hits) {
        CAPTURE(hit.pad);
        CAPTURE(hit.cymbal);
        DrumInputs in;
        in.pads[hit.pad] = 100;
        in.cymbals[hit.cymbal] = 40;
        CheckDrums(in, RoundTrip(in));
    }
}

TEST_CASE("two pads, and two cymbals, decode back") {
    DrumInputs in;
    in.pads[kRedPad] = 120;
    in.pads[kGreenPad] = 30;
    CheckDrums(in, RoundTrip(in));
    in = {};
    in.cymbals[kYellowCymbal] = 64;
    in.cymbals[kGreenCymbal] = 100;
    CheckDrums(in, RoundTrip(in));
    in = {};
    in.cymbals[kYellowCymbal] = 64;
    in.cymbals[kBlueCymbal] = 12;
    in.kick1 = true;
    CheckDrums(in, RoundTrip(in));
}

TEST_CASE("a pad and cymbal of one color decode back, the cymbal's velocity from red") {
    for (int cymbal = 0; cymbal < kCymbalCount; cymbal++) {
        CAPTURE(cymbal);
        const DrumPad pad = cymbal == kYellowCymbal ? kYellowPad
                            : cymbal == kBlueCymbal ? kBluePad
                                                    : kGreenPad;
        DrumInputs in;
        in.pads[pad] = 127;
        in.cymbals[cymbal] = 5;
        CheckDrums(in, RoundTrip(in));
    }
}

TEST_CASE("face buttons without a flag are menu presses, not hits") {
    DrumInputs in;
    in.nav.a = true;
    in.nav.b = true;
    in.nav.start = true;
    in.nav.dpad_up = true;
    const DrumInputs out = RoundTrip(in);
    CHECK(out.pads == std::array<uint8_t, kPadCount>{});
    CHECK(out.cymbals == std::array<uint8_t, kCymbalCount>{});
    CHECK(SameNav(out.nav, in.nav));
}

TEST_CASE("an RB1 kit reads pads without velocity or cymbals") {
    const Caps360 rb1 = DrumCaps(false);
    CHECK_FALSE(IsRb2Drums(rb1));
    CHECK(IsRb2Drums(DrumCaps()));

    for (int pad = 0; pad < kPadCount; pad++) {
        CAPTURE(pad);
        DrumInputs in;
        in.pads[pad] = 20;
        DrumInputs out = DecodeDrums(EncodeDrums(in), rb1);
        DrumInputs want;
        want.pads[pad] = 127;
        CheckDrums(want, out);
    }

    // an RB1 kit's own report: face buttons and the kick, no flags, no axes
    Gamepad360 g;
    g.buttons = xbox::kButtonB | xbox::kButtonX | xbox::kLeftShoulder | xbox::kDpadUp;
    const DrumInputs out = DecodeDrums(g, rb1);
    DrumInputs want;
    want.pads[kRedPad] = 127;
    want.pads[kBluePad] = 127;
    want.kick1 = true;
    CheckDrums(want, out);
    CHECK(out.nav.dpad_up);
}

namespace {

// encode, decode, and the decoded inputs encode the same again
KeysInputs RoundTrip(const KeysInputs& in) {
    const Gamepad360 g = EncodeKeys(in);
    const KeysInputs out = DecodeKeys(g);
    const Gamepad360 again = EncodeKeys(out);
    CHECK(again.buttons == g.buttons);
    CHECK(again.left_trigger == g.left_trigger);
    CHECK(again.right_trigger == g.right_trigger);
    CHECK(again.thumb_lx == g.thumb_lx);
    CHECK(again.thumb_ly == g.thumb_ly);
    CHECK(again.thumb_rx == g.thumb_rx);
    CHECK(again.thumb_ry == g.thumb_ry);
    return out;
}

}

TEST_CASE("each keytar key decodes back at every velocity") {
    for (int key = 0; key < kKeyCount; key++) {
        for (int velocity = 1; velocity <= 127; velocity++) {
            CAPTURE(key);
            CAPTURE(velocity);
            KeysInputs in;
            in.keys[key] = static_cast<uint8_t>(velocity);
            CHECK(RoundTrip(in).keys == in.keys);
        }
    }
}

TEST_CASE("five keytar keys decode back with their own velocities, lowest first") {
    KeysInputs in;
    in.keys[0] = 10;
    in.keys[7] = 20;
    in.keys[8] = 30;
    in.keys[16] = 40;
    in.keys[24] = 50;
    CHECK(RoundTrip(in).keys == in.keys);

    in = {};
    in.keys[3] = 127;
    in.keys[12] = 1;
    in.keys[23] = 64;
    CHECK(RoundTrip(in).keys == in.keys);
}

TEST_CASE("keytar keys past the fifth decode as held at 127") {
    KeysInputs in;
    for (int key = 0; key < 8; key++) in.keys[key] = static_cast<uint8_t>(10 + key);
    in.keys[24] = 90;
    const KeysInputs out = RoundTrip(in);
    for (int key = 0; key < 5; key++) {
        CAPTURE(key);
        CHECK(out.keys[key] == in.keys[key]);
    }
    for (int key : {5, 6, 7, 24}) {
        CAPTURE(key);
        CHECK(out.keys[key] == 127);
    }
    for (int key = 8; key < 24; key++) {
        CAPTURE(key);
        CHECK(out.keys[key] == 0);
    }
}

TEST_CASE("keytar overdrive and the menu buttons decode back") {
    KeysInputs in;
    in.overdrive = true;
    KeysInputs out = RoundTrip(in);
    CHECK(out.overdrive);
    CHECK(out.keys == std::array<uint8_t, kKeyCount>{});

    in = {};
    in.nav.a = true;
    in.nav.start = true;
    in.nav.back = true;
    in.nav.dpad_down = true;
    in.keys[12] = 80;
    out = RoundTrip(in);
    CHECK(SameNav(out.nav, in.nav));
    CHECK_FALSE(out.overdrive);
    CHECK(out.keys == in.keys);

    // nothing held: the pedal bits alone are no key and no overdrive
    out = DecodeKeys(EncodeKeys({}));
    CHECK(out.keys == std::array<uint8_t, kKeyCount>{});
    CHECK_FALSE(out.overdrive);
}

TEST_CASE("a keytar key reported without a velocity reads at 127") {
    Gamepad360 g;
    g.left_trigger = 0x80;
    CHECK(DecodeKeys(g).keys[0] == 127);
}
