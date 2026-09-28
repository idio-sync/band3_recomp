#pragma once
// What RB3 does with an Xbox 360 instrument's state, written out independently
// of src/ so the tests check against the game rather than against themselves:
// - fret and pad slots from RB3's config/beatmatch_controller.dta (the copy in
//   Rock Band 3 Deluxe's repo, _ark/config/beatmatch_controller.dta)
// - model detection from ReadSingleXinputJoypad and the SetupHX* functions in
//   the rb3-xenon decomp (src/system/os/Joypad_Xinput.cpp, Joypad_Xbox.cpp)

#include <cstdint>
#include "src/Input/instruments.h"

namespace rb3 {

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
constexpr uint16_t kA = 0x1000;
constexpr uint16_t kB = 0x2000;
constexpr uint16_t kX = 0x4000;
constexpr uint16_t kY = 0x8000;

inline uint16_t u16(int16_t v) { return static_cast<uint16_t>(v); }

// RB3's JoypadButton numbers for the Xbox buttons (kPad_Xbox_* in Joypad.h),
// which beatmatch_controller.dta's slot tables use
inline int JoypadButtonFor(uint16_t xinput_button) {
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
inline int GuitarSlot(int joypad_button) {
    constexpr int kSlots[][2] = {{6, 0}, {5, 1}, {4, 2}, {7, 3}, {2, 4}};
    for (const auto& s : kSlots) {
        if (s[0] == joypad_button) return s[1];
    }
    return -1;
}

// hx_drums_xbox_rb2 (slots 2 0 5 1 4 2 7 3 6 4): kick 0, red 1, yellow 2, blue 3, green 4
inline int DrumSlot(int joypad_button) {
    constexpr int kSlots[][2] = {{2, 0}, {5, 1}, {4, 2}, {7, 3}, {6, 4}};
    for (const auto& s : kSlots) {
        if (s[0] == joypad_button) return s[1];
    }
    return -1;
}

// the face or shoulder buttons in `buttons` that carry a slot, without the
// flags and d-pad
inline uint16_t SlotButtons(uint16_t buttons) {
    return buttons & (kA | kB | kX | kY | kLeftShoulder);
}

// RB3's model checks, as the decomp has them
inline bool SeesRb2Guitar(const band3::input::Caps360& c) {
    bool wireless = c.flags & 0x2;
    return wireless && ((c.flags & 1) || c.gamepad.thumb_rx >= 0x100);
}
inline bool SeesMidiProAdapter(const band3::input::Caps360& c) {
    return u16(c.gamepad.thumb_lx) == 0x1BAD;
}
inline bool SeesRb2Drums(const band3::input::Caps360& c) {
    return (c.flags & 1) || c.gamepad.thumb_rx >= 0x100;
}
inline bool SeesKeytar(const band3::input::Caps360& c) {
    return (u16(c.gamepad.thumb_ly) & 0xFFF0) != 0x1730;  // 0x173x is a MIDI Pro Adapter
}

}
