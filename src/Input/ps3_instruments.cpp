#include "ps3_instruments.h"

namespace band3::input {

namespace {

using namespace xbox;

// report bytes, after any report ID
constexpr size_t kReportSize = 27;
constexpr size_t kButtonsLow = 0;
constexpr size_t kButtonsHigh = 1;
constexpr size_t kHat = 2;
constexpr size_t kRightStickX = 5;
constexpr size_t kRightStickY = 6;
// the drum kits' velocities, in the pressure bytes of the PS3 shoulder buttons
constexpr size_t kYellowVelocity = 11;
constexpr size_t kRedVelocity = 12;
constexpr size_t kGreenVelocity = 13;
constexpr size_t kBlueVelocity = 14;

// button bits
constexpr uint16_t kSquare = 0x0001;
constexpr uint16_t kCross = 0x0002;
constexpr uint16_t kCircle = 0x0004;
constexpr uint16_t kTriangle = 0x0008;
constexpr uint16_t kShoulder4 = 0x0010;  // guitar orange, drums kick 1
constexpr uint16_t kShoulder5 = 0x0020;  // guitar tilt, drums kick 2
constexpr uint16_t kShoulder6 = 0x0040;  // guitar solo flag
constexpr uint16_t kSelect = 0x0100;
constexpr uint16_t kStartButton = 0x0200;
constexpr uint16_t kL3 = 0x0400;  // drums pad flag
constexpr uint16_t kR3 = 0x0800;  // drums cymbal flag

// the whammy and pickup switch report this after a moment at rest
constexpr uint8_t kAtRest = 0x7F;

constexpr Ps3InstrumentId kKnown[] = {
    {0x12BA, 0x0200, "PS3 Rock Band guitar"},
    {0x12BA, 0x0210, "PS3 Rock Band drums"},
    {0x12BA, 0x0218, "PS3 MIDI Pro Adapter (drums)"},
    {0x1BAD, 0x0004, "Wii Rock Band guitar"},
    {0x1BAD, 0x3010, "Wii Rock Band 2 guitar"},
    {0x1BAD, 0x0005, "Wii Rock Band drums"},
    {0x1BAD, 0x3110, "Wii Rock Band 2 drums"},
    {0x1BAD, 0x3138, "Wii MIDI Pro Adapter (drums)"},
};

// the hat switch: 0 up, clockwise to 7 up-left; anything else is centered
struct Dpad {
    bool up, right, down, left;
};

Dpad ReadHat(uint8_t hat) {
    hat &= 0x0F;
    return {hat == 7 || hat == 0 || hat == 1, hat >= 1 && hat <= 3, hat >= 3 && hat <= 5,
            hat >= 5 && hat <= 7};
}

uint16_t DpadButtons(const Dpad& d) {
    return (d.up ? kDpadUp : 0) | (d.down ? kDpadDown : 0) | (d.left ? kDpadLeft : 0) |
           (d.right ? kDpadRight : 0);
}

}

std::span<const Ps3InstrumentId> KnownPs3Instruments() { return kKnown; }

std::optional<Ps3Instrument> IdentifyPs3Instrument(uint16_t vendor, uint16_t product,
                                                   uint16_t release) {
    if (vendor == 0x12BA) {
        if (product == 0x0200) return Ps3Instrument::kGuitar;
        // RB1 PS3 kits report release 0x1000, RB2 ones 0x0200
        if (product == 0x0210) return release == 0x1000 ? Ps3Instrument::kDrumsRb1
                                                        : Ps3Instrument::kDrums;
        if (product == 0x0218) return Ps3Instrument::kDrums;
    } else if (vendor == 0x1BAD) {
        if (product == 0x0004 || product == 0x3010) return Ps3Instrument::kGuitar;
        if (product == 0x0005) return Ps3Instrument::kDrumsRb1;
        if (product == 0x3110 || product == 0x3138) return Ps3Instrument::kDrums;
    }
    return std::nullopt;
}

Caps360 Ps3InstrumentCaps(Ps3Instrument instrument) {
    switch (instrument) {
    case Ps3Instrument::kGuitar: return GuitarCaps(false);
    case Ps3Instrument::kDrumsRb1: return DrumCaps(false);
    case Ps3Instrument::kDrums: return DrumCaps(true);
    }
    return GuitarCaps(false);
}

std::optional<Gamepad360> Ps3InstrumentTranslator::Translate(std::span<const uint8_t> report) {
    // a report ID is only ever 0 for these, and some platforms keep it
    if (report.size() == kReportSize + 1 && report[0] == 0) report = report.subspan(1);
    if (report.size() < kReportSize) return std::nullopt;

    const uint16_t b = static_cast<uint16_t>(report[kButtonsLow] | report[kButtonsHigh] << 8);
    const Dpad dpad = ReadHat(report[kHat]);

    if (instrument_ == Ps3Instrument::kGuitar) {
        GuitarInputs in;
        in.nav.start = b & kStartButton;
        in.nav.back = b & kSelect;
        in.nav.dpad_left = dpad.left;
        in.nav.dpad_right = dpad.right;
        in.strum_up = dpad.up;
        in.strum_down = dpad.down;
        in.frets[kGreen] = b & kCross;
        in.frets[kRed] = b & kCircle;
        in.frets[kYellow] = b & kTriangle;
        in.frets[kBlue] = b & kSquare;
        in.frets[kOrange] = b & kShoulder4;
        in.solo = b & kShoulder6;
        in.tilt = b & kShoulder5;
        if (report[kRightStickX] != kAtRest) whammy_ = report[kRightStickX];
        if (report[kRightStickY] != kAtRest) pickup_ = report[kRightStickY];
        in.whammy = whammy_ / 255.0f;
        in.pickup = pickup_;
        return EncodeGuitar(in);
    }

    // Drum kits send the same flags as Xbox 360 kits (a color, the pad or cymbal
    // flag, d-pad up/down for the yellow/blue cymbals), so they are passed across
    // flag for flag and RB3 sorts out pads from cymbals itself, as with a 360 kit.
    Gamepad360 g;
    g.buttons = DpadButtons(dpad);
    if (b & kSquare) g.buttons |= kButtonX;
    if (b & kCross) g.buttons |= kButtonA;
    if (b & kCircle) g.buttons |= kButtonB;
    if (b & kTriangle) g.buttons |= kButtonY;
    if (b & kShoulder4) g.buttons |= kLeftShoulder;
    if (b & kShoulder5) g.buttons |= kLeftThumb;
    if (b & kL3) g.buttons |= kRightThumb;
    if (b & kR3) g.buttons |= kRightShoulder;
    if (b & kSelect) g.buttons |= kBack;
    if (b & kStartButton) g.buttons |= kStart;

    // both inverted (0 is the hardest hit), 8 bits here and 15 on the 360, where
    // yellow and green also set the top bit while their color is hit
    auto velocity = [&](size_t offset, uint16_t color, bool high_bit) -> int16_t {
        const uint8_t raw = report[offset];
        if (raw == 0 && !(g.buttons & color)) return 0;
        uint16_t v = static_cast<uint16_t>(raw * 0x7FFF / 0xFF);
        if (high_bit) v |= 0x8000;
        return static_cast<int16_t>(v);
    };
    if (instrument_ == Ps3Instrument::kDrums) {
        g.thumb_lx = velocity(kRedVelocity, kButtonB, false);
        g.thumb_ly = velocity(kYellowVelocity, kButtonY, true);
        g.thumb_rx = velocity(kBlueVelocity, kButtonX, false);
        g.thumb_ry = velocity(kGreenVelocity, kButtonA, true);
    }
    return g;
}

}
