#include "ps3_instruments.h"
#include "hat_switch.h"

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

// a report ID is only ever 0 for these, and some platforms keep it
std::optional<std::span<const uint8_t>> Body(std::span<const uint8_t> report) {
    if (report.size() == kReportSize + 1 && report[0] == 0) report = report.subspan(1);
    if (report.size() < kReportSize) return std::nullopt;
    return report;
}

uint16_t Buttons(std::span<const uint8_t> r) {
    return static_cast<uint16_t>(r[kButtonsLow] | r[kButtonsHigh] << 8);
}

}

std::optional<Gamepad360> TranslatePs3Guitar(std::span<const uint8_t> report,
                                             Ps3GuitarState& state) {
    const auto r = Body(report);
    if (!r) return std::nullopt;
    const uint16_t b = Buttons(*r);
    const HatSwitch hat = ReadHatSwitch((*r)[kHat]);

    GuitarInputs in;
    in.nav.start = b & kStartButton;
    in.nav.back = b & kSelect;
    in.nav.dpad_left = hat.left;
    in.nav.dpad_right = hat.right;
    in.strum_up = hat.up;
    in.strum_down = hat.down;
    in.frets[kGreen] = b & kCross;
    in.frets[kRed] = b & kCircle;
    in.frets[kYellow] = b & kTriangle;
    in.frets[kBlue] = b & kSquare;
    in.frets[kOrange] = b & kShoulder4;
    in.solo = b & kShoulder6;
    in.tilt = (b & kShoulder5) ? 1.0f : 0.0f;
    if ((*r)[kRightStickX] != kAtRest) state.whammy = (*r)[kRightStickX];
    if ((*r)[kRightStickY] != kAtRest) state.pickup = (*r)[kRightStickY];
    in.whammy = state.whammy / 255.0f;
    in.pickup = state.pickup;
    return EncodeGuitar(in);
}

std::optional<Gamepad360> TranslatePs3Drums(std::span<const uint8_t> report, bool has_velocity) {
    const auto r = Body(report);
    if (!r) return std::nullopt;
    const uint16_t b = Buttons(*r);

    // Drum kits send the same flags as Xbox 360 kits (a color, the pad or cymbal
    // flag, d-pad up/down for the yellow/blue cymbals), so they are passed across
    // flag for flag and RB3 sorts out pads from cymbals itself, as with a 360 kit.
    Gamepad360 g;
    g.buttons = DpadButtons(ReadHatSwitch((*r)[kHat]));
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
    if (!has_velocity) return g;

    // both inverted (0 is the hardest hit), 8 bits here and 15 on the 360, where
    // yellow and green also set the top bit while their color is hit
    auto velocity = [&](size_t offset, uint16_t color, bool high_bit) -> int16_t {
        const uint8_t raw = (*r)[offset];
        if (raw == 0 && !(g.buttons & color)) return 0;
        uint16_t v = static_cast<uint16_t>(raw * 0x7FFF / 0xFF);
        if (high_bit) v |= 0x8000;
        return static_cast<int16_t>(v);
    };
    g.thumb_lx = velocity(kRedVelocity, kButtonB, false);
    g.thumb_ly = velocity(kYellowVelocity, kButtonY, true);
    g.thumb_rx = velocity(kBlueVelocity, kButtonX, false);
    g.thumb_ry = velocity(kGreenVelocity, kButtonA, true);
    return g;
}

}
