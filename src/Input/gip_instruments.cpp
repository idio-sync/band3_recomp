#include "gip_instruments.h"
#include <algorithm>
#include <iterator>

namespace band3::input {

namespace {

// Offsets count the report ID at byte 0, so the data's byte n is at n + 1.

// the first button byte
constexpr uint8_t kMenu = 0x04;
constexpr uint8_t kView = 0x08;
constexpr uint8_t kA = 0x10;  // drums: the green pad (and a guitar's green fret flag)
constexpr uint8_t kB = 0x20;  // drums: the red pad
constexpr uint8_t kX = 0x40;  // drums: the blue pad, on PDP kits only
constexpr uint8_t kY = 0x80;  // drums: the yellow pad, on PDP kits only
// the second
constexpr uint8_t kDpadUp = 0x01;  // a guitar's strum up
constexpr uint8_t kDpadDown = 0x02;
constexpr uint8_t kDpadLeft = 0x04;
constexpr uint8_t kDpadRight = 0x08;
constexpr uint8_t kKick1 = 0x10;  // drums; a guitar's orange fret flag
constexpr uint8_t kKick2 = 0x20;

// the pickup switch reports its notch as 0-4 in the high nibble; these are the
// values an Xbox 360 guitar reports at each notch (PlasticBand's 5-Fret
// Guitar/Rock Band/General Notes), as in ps4_instruments.cpp
constexpr uint8_t kPickupNotches[] = {0x17, 0x4B, 0x79, 0xAB, 0xE0};

bool IsInputState(std::span<const uint8_t> report, size_t last_offset) {
    return report.size() > last_offset && report[0] == kGipInputReport;
}

void ReadNav(uint8_t buttons1, uint8_t buttons2, NavInputs& nav) {
    nav.start = buttons1 & kMenu;
    nav.back = buttons1 & kView;
    nav.dpad_left = buttons2 & kDpadLeft;
    nav.dpad_right = buttons2 & kDpadRight;
}

// 0-15, harder hits higher (the kits seem to stop at 7), to band3's 1-127; 0
// is no hit. RB4InstrumentMapper scales the nibble to a byte (x 0x11) before
// inverting it into the 360's velocity, as ps4_instruments.cpp takes a byte.
uint8_t Velocity(uint8_t nibble) {
    if (nibble == 0) return 0;
    return static_cast<uint8_t>(std::clamp((nibble * 0x11 + 1) / 2, 1, 127));
}

}

std::optional<Gamepad360> TranslateXboxOneGuitar(std::span<const uint8_t> report) {
    constexpr size_t kButtons1 = 1, kButtons2 = 2, kTilt = 3, kWhammy = 4, kPickup = 5;
    constexpr size_t kFrets = 6, kSoloFrets = 7;
    if (!IsInputState(report, kSoloFrets)) return std::nullopt;

    const uint8_t buttons2 = report[kButtons2];
    GuitarInputs in;
    ReadNav(report[kButtons1], buttons2, in.nav);
    in.strum_up = buttons2 & kDpadUp;
    in.strum_down = buttons2 & kDpadDown;

    // The fret bytes, not the fret flags: on the Riffmaster the joystick click
    // shares the solo flag. Upper and solo frets can't both be told apart on a
    // 360 guitar either, so a solo fret wins.
    const uint8_t upper = report[kFrets] & 0x1F;
    const uint8_t solo = report[kSoloFrets] & 0x1F;
    for (int f = 0; f < kFretCount; f++) {
        in.frets[f] = ((upper | solo) >> f) & 1;
    }
    in.solo = solo != 0;

    in.whammy = report[kWhammy] / 255.0f;
    in.tilt = report[kTilt] / 255.0f;
    const uint8_t notch = report[kPickup] >> 4;
    in.pickup = notch < std::size(kPickupNotches) ? kPickupNotches[notch] : kPickupNotches[0];
    return EncodeGuitar(in);
}

std::optional<Gamepad360> TranslateXboxOneDrums(std::span<const uint8_t> report) {
    constexpr size_t kButtons1 = 1, kButtons2 = 2;
    // velocity nibbles, low then high
    constexpr size_t kYellowRedPadsAt = 3, kGreenBluePadsAt = 4;
    constexpr size_t kBlueYellowCymbalsAt = 5, kGreenCymbalAt = 6;
    if (!IsInputState(report, kGreenCymbalAt)) return std::nullopt;

    // Pads and cymbals each have their own velocity; any above zero is a hit.
    // The kit also presses the face button of some colors, which only counts
    // as a button when nothing of its color was hit.
    DrumInputs in;
    in.pads[kYellowPad] = Velocity(report[kYellowRedPadsAt] & 0x0F);
    in.pads[kRedPad] = Velocity(report[kYellowRedPadsAt] >> 4);
    in.pads[kGreenPad] = Velocity(report[kGreenBluePadsAt] & 0x0F);
    in.pads[kBluePad] = Velocity(report[kGreenBluePadsAt] >> 4);
    in.cymbals[kBlueCymbal] = Velocity(report[kBlueYellowCymbalsAt] & 0x0F);
    in.cymbals[kYellowCymbal] = Velocity(report[kBlueYellowCymbalsAt] >> 4);
    in.cymbals[kGreenCymbal] = Velocity(report[kGreenCymbalAt] >> 4);

    const uint8_t buttons1 = report[kButtons1];
    const uint8_t buttons2 = report[kButtons2];
    const bool red = in.pads[kRedPad];
    const bool yellow = in.pads[kYellowPad] || in.cymbals[kYellowCymbal];
    const bool blue = in.pads[kBluePad] || in.cymbals[kBlueCymbal];
    const bool green = in.pads[kGreenPad] || in.cymbals[kGreenCymbal];
    const bool any_cymbal =
        in.cymbals[kYellowCymbal] || in.cymbals[kBlueCymbal] || in.cymbals[kGreenCymbal];

    ReadNav(buttons1, buttons2, in.nav);
    in.nav.a = (buttons1 & kA) && !green;
    in.nav.b = (buttons1 & kB) && !red;
    in.nav.x = (buttons1 & kX) && !blue;
    in.nav.y = (buttons1 & kY) && !yellow;
    // up/down mark the yellow/blue cymbals on the 360, so they are left to the
    // cymbals while one is hit
    in.nav.dpad_up = (buttons2 & kDpadUp) && !any_cymbal;
    in.nav.dpad_down = (buttons2 & kDpadDown) && !any_cymbal;
    in.kick1 = buttons2 & kKick1;
    in.kick2 = buttons2 & kKick2;
    return EncodeDrums(in);
}

}
