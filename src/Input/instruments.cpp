#include "instruments.h"
#include <algorithm>

namespace band3::input {

namespace {

constexpr uint16_t kDpadUp = 0x0001;
constexpr uint16_t kDpadDown = 0x0002;
constexpr uint16_t kDpadLeft = 0x0004;
constexpr uint16_t kDpadRight = 0x0008;
constexpr uint16_t kStart = 0x0010;
constexpr uint16_t kBack = 0x0020;
constexpr uint16_t kLeftThumb = 0x0040;
constexpr uint16_t kRightThumb = 0x0080;
constexpr uint16_t kLeftShoulder = 0x0100;
constexpr uint16_t kRightShoulder = 0x0200;
constexpr uint16_t kButtonA = 0x1000;
constexpr uint16_t kButtonB = 0x2000;
constexpr uint16_t kButtonX = 0x4000;
constexpr uint16_t kButtonY = 0x8000;

constexpr uint16_t kCapsWireless = 0x0002;
// RB2-and-later drum kits set this; RB3 reads it as "RB2 drums"
constexpr uint16_t kCapsForceFeedback = 0x0001;

uint16_t NavButtons(const NavInputs& n) {
    uint16_t b = 0;
    if (n.a) b |= kButtonA;
    if (n.b) b |= kButtonB;
    if (n.x) b |= kButtonX;
    if (n.y) b |= kButtonY;
    if (n.start) b |= kStart;
    if (n.back) b |= kBack;
    if (n.dpad_up) b |= kDpadUp;
    if (n.dpad_down) b |= kDpadDown;
    if (n.dpad_left) b |= kDpadLeft;
    if (n.dpad_right) b |= kDpadRight;
    return b;
}

uint8_t Clamp7(uint8_t v) { return std::min<uint8_t>(v, 127); }

// the capability values RB3 checks, set to what a real device of each kind
// reports; stick axes double as hardware IDs
Caps360 BaseCaps(uint8_t sub_type, uint16_t flags) {
    Caps360 c;
    c.sub_type = sub_type;
    c.flags = flags;
    c.gamepad.buttons = 0xF3FF;
    c.gamepad.left_trigger = 0xFF;
    c.gamepad.right_trigger = 0xFF;
    return c;
}

}

bool IsRb3InstrumentSubtype(uint8_t subtype) {
    switch (subtype) {
    case kSubtypeGuitar:
    case kSubtypeGuitarAlternate:
    case kSubtypeDrums:
    case 9:
    case kSubtypeGuitarBass:
    case kSubtypeKeytar:
    case kSubtypeProGuitar:
        return true;
    default:
        return false;
    }
}

// Guitar

Caps360 GuitarCaps() {
    // wireless with sThumbRX >= 0x100 reads as an RB2-or-later guitar. sThumbLX
    // must not be 0x1BAD, which would make RB3 query it as a MIDI Pro Adapter.
    Caps360 c = BaseCaps(kSubtypeGuitar, kCapsWireless);
    c.gamepad.thumb_rx = 0x0200;
    return c;
}

Gamepad360 EncodeGuitar(const GuitarInputs& in) {
    Gamepad360 g;
    g.buttons = NavButtons(in.nav);

    // frets follow the 360 face button colors. PlasticBand's table swaps yellow
    // and blue, but its struct (and the controller's colors) put yellow on Y.
    static constexpr uint16_t kFretButtons[kFretCount] = {kButtonA, kButtonB, kButtonY, kButtonX,
                                                          kLeftShoulder};
    bool any_fret = false;
    for (int f = 0; f < kFretCount; f++) {
        if (in.frets[f]) {
            g.buttons |= kFretButtons[f];
            any_fret = true;
        }
    }
    if (in.solo && any_fret) g.buttons |= kLeftThumb;
    if (in.strum_up) g.buttons |= kDpadUp;
    if (in.strum_down) g.buttons |= kDpadDown;

    g.left_trigger = in.pickup;
    // -32768 released to 32767 fully pressed
    float w = std::clamp(in.whammy, 0.0f, 1.0f);
    g.thumb_rx = static_cast<int16_t>(-32768 + static_cast<int>(w * 65535.0f));
    g.thumb_ry = in.tilt ? 32767 : 0;
    return g;
}

// Drums

Caps360 DrumCaps() {
    // force feedback marks RB2-or-later kits; sThumbLX 0x1BAD would be a MIDI
    // Pro Adapter
    Caps360 c = BaseCaps(kSubtypeDrums, kCapsForceFeedback | kCapsWireless);
    c.gamepad.thumb_rx = 0x0200;
    return c;
}

Gamepad360 EncodeDrums(const DrumInputs& in) {
    Gamepad360 g;
    g.buttons = NavButtons(in.nav);

    static constexpr uint16_t kPadButtons[kPadCount] = {kButtonB, kButtonY, kButtonX, kButtonA};
    // each velocity axis belongs to one pad color; yellow and green set the top bit
    static constexpr bool kVelocityHighBit[kPadCount] = {false, true, false, true};
    // cymbals share the pad colors' buttons and velocity axes
    static constexpr DrumPad kCymbalPad[kCymbalCount] = {kYellowPad, kBluePad, kGreenPad};
    static constexpr uint16_t kCymbalDpad[kCymbalCount] = {kDpadUp, kDpadDown, 0};

    std::array<uint8_t, kPadCount> velocity{};
    bool any_pad = false;
    for (int p = 0; p < kPadCount; p++) {
        if (in.pads[p] == 0) continue;
        g.buttons |= kPadButtons[p];
        velocity[p] = Clamp7(in.pads[p]);
        any_pad = true;
    }

    bool any_cymbal = false;
    for (int c = 0; c < kCymbalCount; c++) {
        if (in.cymbals[c] == 0) continue;
        DrumPad pad = kCymbalPad[c];
        g.buttons |= kPadButtons[pad] | kCymbalDpad[c];
        // a pad and cymbal of one color hit together: the cymbal's velocity goes
        // in the red axis, without the red button
        if (velocity[pad] != 0) {
            if (velocity[kRedPad] == 0) velocity[kRedPad] = Clamp7(in.cymbals[c]);
        } else {
            velocity[pad] = Clamp7(in.cymbals[c]);
        }
        any_cymbal = true;
    }

    if (any_pad) g.buttons |= kRightThumb;
    if (any_cymbal) g.buttons |= kRightShoulder;
    if (in.kick1) g.buttons |= kLeftShoulder;
    if (in.kick2) g.buttons |= kLeftThumb;

    // inverted for RB1 kit compatibility: 0x0000 is the hardest hit, 0x7FFF the softest
    std::array<int16_t, kPadCount> axis{};
    for (int p = 0; p < kPadCount; p++) {
        if (velocity[p] == 0) continue;
        uint16_t v = static_cast<uint16_t>((127 - velocity[p]) * 0x7FFF / 126);
        if (kVelocityHighBit[p]) v |= 0x8000;
        axis[p] = static_cast<int16_t>(v);
    }
    g.thumb_lx = axis[kRedPad];
    g.thumb_ly = axis[kYellowPad];
    g.thumb_rx = axis[kBluePad];
    g.thumb_ry = axis[kGreenPad];
    return g;
}

// Keys

Caps360 KeysCaps() {
    // stick hardware IDs 1BAD:1330 rev 4; sThumbLY 0x173x would be a MIDI Pro Adapter
    Caps360 c = BaseCaps(kSubtypeKeytar, kCapsWireless);
    c.gamepad.thumb_lx = 0x1BAD;
    c.gamepad.thumb_ly = 0x1330;
    c.gamepad.thumb_rx = 0x0004;
    return c;
}

Gamepad360 EncodeKeys(const KeysInputs& in) {
    Gamepad360 g;
    g.buttons = NavButtons(in.nav);

    uint16_t lx = 0, ly = 0, rx = 0, ry = 0;
    // keys 1-8 in the left trigger and 9-16 in the right, lowest key in the top
    // bit; 17-24 in the low byte of sThumbLX the same way, 25 in its top bit
    for (int k = 0; k < kKeyCount; k++) {
        if (in.keys[k] == 0) continue;
        if (k < 8) {
            g.left_trigger |= static_cast<uint8_t>(0x80 >> k);
        } else if (k < 16) {
            g.right_trigger |= static_cast<uint8_t>(0x80 >> (k - 8));
        } else if (k < 24) {
            lx |= static_cast<uint16_t>(0x80 >> (k - 16));
        } else {
            lx |= 0x8000;
        }
    }

    // up to five velocities, paired with the held keys from the lowest up
    int slot = 0;
    for (int k = 0; k < kKeyCount && slot < 5; k++) {
        if (in.keys[k] == 0) continue;
        uint16_t v = Clamp7(in.keys[k]);
        switch (slot++) {
        case 0: lx |= static_cast<uint16_t>(v << 8); break;
        case 1: ly |= v; break;
        case 2: ly |= static_cast<uint16_t>(v << 8); break;
        case 3: rx |= v; break;
        case 4: rx |= static_cast<uint16_t>(v << 8); break;
        }
    }

    if (in.overdrive) ry |= 0x0080;
    // the analog pedal bits all read 1 with nothing plugged in
    ry |= 0x7F00;

    g.thumb_lx = static_cast<int16_t>(lx);
    g.thumb_ly = static_cast<int16_t>(ly);
    g.thumb_rx = static_cast<int16_t>(rx);
    g.thumb_ry = static_cast<int16_t>(ry);
    return g;
}

// Pro Guitar

Caps360 ProGuitarCaps(ProGuitarModel model) {
    // sThumbLY picks the model: 0x153x a 22-fret Squier, 0x143x a Mustang
    Caps360 c = BaseCaps(kSubtypeProGuitar, kCapsWireless);
    c.gamepad.thumb_ly = model == ProGuitarModel::kSquier ? 0x1530 : 0x1430;
    return c;
}

Gamepad360 EncodeProGuitar(const ProGuitarInputs& in) {
    Gamepad360 g;
    g.buttons = NavButtons(in.nav);

    auto fret = [&](GuitarString s) -> uint16_t {
        return std::min<uint8_t>(in.frets[s], kMaxProFret) & 0x1F;
    };
    auto velocity = [&](GuitarString s) -> uint16_t { return Clamp7(in.velocities[s]); };
    auto color = [&](Fret f) -> uint16_t { return in.colors[f] ? 1 : 0; };

    // low E, A and D frets across both triggers, left trigger as the low byte
    uint16_t triggers = static_cast<uint16_t>(fret(kStringLowE) | fret(kStringA) << 5 |
                                              fret(kStringD) << 10);
    g.left_trigger = static_cast<uint8_t>(triggers & 0xFF);
    g.right_trigger = static_cast<uint8_t>(triggers >> 8);

    uint16_t lx = static_cast<uint16_t>(fret(kStringG) | fret(kStringB) << 5 |
                                        fret(kStringHighE) << 10 | (in.solo ? 0x8000 : 0));
    uint16_t ly = static_cast<uint16_t>(velocity(kStringLowE) | color(kGreen) << 7 |
                                        velocity(kStringA) << 8 | color(kRed) << 15);
    uint16_t rx = static_cast<uint16_t>(velocity(kStringD) | color(kYellow) << 7 |
                                        velocity(kStringG) << 8 | color(kBlue) << 15);
    uint16_t ry = static_cast<uint16_t>(velocity(kStringB) | color(kOrange) << 7 |
                                        velocity(kStringHighE) << 8);

    g.thumb_lx = static_cast<int16_t>(lx);
    g.thumb_ly = static_cast<int16_t>(ly);
    g.thumb_rx = static_cast<int16_t>(rx);
    g.thumb_ry = static_cast<int16_t>(ry);
    return g;
}

}
