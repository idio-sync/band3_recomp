#pragma once
// What RB3 does with an Xbox 360 instrument's state, written out independently
// of src/ so the tests check against the game rather than against themselves:
// - fret and pad slots from RB3's config/beatmatch_controller.dta (the copy in
//   Rock Band 3 Deluxe's repo, _ark/config/beatmatch_controller.dta)
// - model detection from ReadSingleXinputJoypad and the SetupHX* functions in
//   the rb3-xenon decomp (src/system/os/Joypad_Xinput.cpp, Joypad_Xbox.cpp)
// - Pro Keys and Pro Guitar from UsbMidiKeyboard::Poll and UsbMidiGuitar::Poll
//   (src/system/os/UsbMidiKeyboard.cpp, UsbMidiGuitar.cpp)

#include <array>
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

// What UsbMidiKeyboard::Poll takes from ProKeysData. Key k (0 = C1) is bit
// 7 - k % 8 of byte k / 8; each newly pressed key takes the velocity of its rank
// among the held keys, from the bottom (GetSlottedKeyVelocityFromExtended: rank
// n is byte 2 + n's low 7 bits, up to 5). Bytes 8 and 9 are bitfields, read from
// the top bit down as the Xbox 360 compiler lays them out.
struct ProKeysRead {
    std::array<bool, 25> pressed{};
    std::array<int, 25> velocity{};
    bool sustain = false;
    bool stomp_pedal = false;
    int expression_pedal = 0;
};

inline ProKeysRead ReadProKeys(const std::array<uint8_t, 16>& d) {
    ProKeysRead r;
    int slot = 1;
    for (int k = 0; k < 25; k++) {
        r.pressed[k] = (d[k / 8] >> (7 - k % 8)) & 1;
        if (!r.pressed[k]) continue;
        r.velocity[k] = slot <= 5 ? d[2 + slot] & 0x7F : 0;
        slot++;
    }
    r.sustain = d[8] >> 7;
    r.stomp_pedal = d[9] >> 7;
    r.expression_pedal = d[9] & 0x7F;
    return r;
}

// What UsbMidiGuitar::Poll takes from ProGuitarData, its bitfields read from
// the top bit down. Strings are the game's numbering: 5 is low E, 0 high E.
struct ProGuitarRead {
    std::array<int, 6> fret{};
    std::array<int, 6> velocity{};
    // the 5-fret color flags, green first (mStringInfos[k].mDown)
    std::array<bool, 5> fret_down{};
    // unk3upper, passed with the fret button messages
    bool solo = false;
};

inline ProGuitarRead ReadProGuitar(const std::array<uint8_t, 16>& d) {
    ProGuitarRead r;
    r.fret[5] = d[0] & 0x1F;
    r.fret[4] = (d[1] & 0x03) << 3 | d[0] >> 5;
    r.fret[3] = d[1] >> 2 & 0x1F;
    r.fret[2] = d[2] & 0x1F;
    r.fret[1] = (d[3] & 0x03) << 3 | d[2] >> 5;
    r.fret[0] = d[3] >> 2 & 0x1F;
    r.solo = d[3] >> 7;
    for (int j = 0; j < 6; j++) r.velocity[j] = d[4 + (5 - j)] & 0x7F;
    for (int k = 0; k < 5; k++) r.fret_down[k] = d[4 + k] >> 7;
    return r;
}

}
