#include "ps4_instruments.h"
#include <algorithm>
#include "hat_switch.h"

namespace band3::input {

namespace {

constexpr uint8_t kReportId = 0x01;

// the button bytes, as on a DualShock 4 / DualSense
// first: the hat switch in the low nibble, then the face buttons
constexpr uint8_t kSquare = 0x10;
constexpr uint8_t kCross = 0x20;
constexpr uint8_t kCircle = 0x40;
constexpr uint8_t kTriangle = 0x80;
// second
constexpr uint8_t kL1 = 0x01;  // drums kick 1 (a guitar's orange is in its fret byte)
constexpr uint8_t kR1 = 0x02;  // drums kick 2
constexpr uint8_t kShare = 0x10;
constexpr uint8_t kOptions = 0x20;

// the pickup switch reports its notch as 0-4; these are the values an Xbox 360
// guitar reports at each notch (PlasticBand's 5-Fret Guitar/Rock Band/General Notes)
constexpr uint8_t kPickupNotches[] = {0x17, 0x4B, 0x79, 0xAB, 0xE0};

// Reports start with report ID 0x01, and offsets here count it. Report reads
// keep it; if one comes without it, reading one byte earlier makes up for that.
class Report {
public:
    Report(std::span<const uint8_t> bytes) : bytes_(bytes) {
        if (!bytes_.empty() && bytes_[0] == kReportId) shift_ = 0;
    }
    bool Covers(size_t offset) const { return offset >= shift_ && offset - shift_ < bytes_.size(); }
    uint8_t operator[](size_t offset) const { return bytes_[offset - shift_]; }

private:
    std::span<const uint8_t> bytes_;
    size_t shift_ = 1;
};

struct GuitarLayout {
    size_t buttons1;
    size_t buttons2;
    size_t pickup;
    size_t whammy;
    size_t tilt;
    size_t frets;
    size_t solo_frets;
};

constexpr GuitarLayout kPs4Guitar{5, 6, 43, 44, 45, 46, 47};
// the PS5 report moves the buttons to the DualSense's place; its pickup offset
// is assumed from the PS4 one, as PlasticBand-Unity does
constexpr GuitarLayout kPs5Guitar{8, 9, 40, 41, 42, 43, 44};

std::optional<Gamepad360> TranslateGuitar(std::span<const uint8_t> bytes,
                                          const GuitarLayout& layout) {
    const Report r(bytes);
    if (!r.Covers(layout.solo_frets)) return std::nullopt;

    const HatSwitch hat = ReadHatSwitch(r[layout.buttons1]);
    const uint8_t buttons2 = r[layout.buttons2];

    GuitarInputs in;
    in.strum_up = hat.up;
    in.strum_down = hat.down;
    in.nav.dpad_left = hat.left;
    in.nav.dpad_right = hat.right;
    in.nav.back = buttons2 & kShare;
    in.nav.start = buttons2 & kOptions;

    // The fret bytes, not the face button flags: on the Riffmaster the
    // joystick click shares the solo flag. Upper and solo frets can't both be
    // told apart on a 360 guitar either, so a solo fret wins.
    const uint8_t upper = r[layout.frets] & 0x1F;
    const uint8_t solo = r[layout.solo_frets] & 0x1F;
    for (int f = 0; f < kFretCount; f++) {
        in.frets[f] = ((upper | solo) >> f) & 1;
    }
    in.solo = solo != 0;

    in.whammy = r[layout.whammy] / 255.0f;
    in.tilt = r[layout.tilt] / 255.0f;
    const uint8_t notch = r[layout.pickup];
    in.pickup = notch < std::size(kPickupNotches) ? kPickupNotches[notch] : kPickupNotches[0];
    return EncodeGuitar(in);
}

// 1-255, hardest at the top, to band3's 1-127; 0 is no hit
uint8_t Velocity(uint8_t raw) {
    if (raw == 0) return 0;
    return static_cast<uint8_t>(std::clamp((raw + 1) / 2, 1, 127));
}

}

std::optional<Gamepad360> TranslatePs4Guitar(std::span<const uint8_t> report) {
    return TranslateGuitar(report, kPs4Guitar);
}

std::optional<Gamepad360> TranslatePs5Guitar(std::span<const uint8_t> report) {
    return TranslateGuitar(report, kPs5Guitar);
}

std::optional<Gamepad360> TranslatePs4Drums(std::span<const uint8_t> bytes) {
    constexpr size_t kButtons1 = 5, kButtons2 = 6;
    // velocity bytes
    constexpr size_t kRedPadAt = 43, kBluePadAt = 44, kYellowPadAt = 45, kGreenPadAt = 46;
    constexpr size_t kYellowCymbalAt = 47, kBlueCymbalAt = 48, kGreenCymbalAt = 49;

    const Report r(bytes);
    if (!r.Covers(kGreenCymbalAt)) return std::nullopt;

    // Pads and cymbals each have their own velocity; any above zero is a hit.
    // The kit also presses the face button of that color, which only counts as
    // a button when nothing of its color was hit.
    DrumInputs in;
    in.pads[kRedPad] = Velocity(r[kRedPadAt]);
    in.pads[kYellowPad] = Velocity(r[kYellowPadAt]);
    in.pads[kBluePad] = Velocity(r[kBluePadAt]);
    in.pads[kGreenPad] = Velocity(r[kGreenPadAt]);
    in.cymbals[kYellowCymbal] = Velocity(r[kYellowCymbalAt]);
    in.cymbals[kBlueCymbal] = Velocity(r[kBlueCymbalAt]);
    in.cymbals[kGreenCymbal] = Velocity(r[kGreenCymbalAt]);

    const uint8_t buttons1 = r[kButtons1];
    const uint8_t buttons2 = r[kButtons2];
    const bool red = in.pads[kRedPad];
    const bool yellow = in.pads[kYellowPad] ||
                        in.cymbals[kYellowCymbal];
    const bool blue = in.pads[kBluePad] || in.cymbals[kBlueCymbal];
    const bool green = in.pads[kGreenPad] ||
                       in.cymbals[kGreenCymbal];
    const bool any_cymbal = in.cymbals[kYellowCymbal] ||
                            in.cymbals[kBlueCymbal] ||
                            in.cymbals[kGreenCymbal];

    in.nav.a = (buttons1 & kCross) && !green;
    in.nav.b = (buttons1 & kCircle) && !red;
    in.nav.x = (buttons1 & kSquare) && !blue;
    in.nav.y = (buttons1 & kTriangle) && !yellow;
    // up/down mark the yellow/blue cymbals on the 360, so they are left to the
    // cymbals while one is hit
    const HatSwitch hat = ReadHatSwitch(buttons1);
    in.nav.dpad_up = hat.up && !any_cymbal;
    in.nav.dpad_down = hat.down && !any_cymbal;
    in.nav.dpad_left = hat.left;
    in.nav.dpad_right = hat.right;
    in.nav.back = buttons2 & kShare;
    in.nav.start = buttons2 & kOptions;
    in.kick1 = buttons2 & kL1;
    in.kick2 = buttons2 & kR1;
    return EncodeDrums(in);
}

}
