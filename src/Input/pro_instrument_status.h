#pragma once
#include <array>
#include <cstdint>
#include "instruments.h"

// What band3 hands RB3 for Pro Keys and Pro Guitar (Hooks/pro_instruments.cpp),
// per pad, for the Instrument Lab: whether the game sees the pad as a keytar or
// pro guitar at all, and the bytes it was given.

namespace band3::input {

// kNumJoypads; pad n reads XInput player n
inline constexpr int kProPads = 4;

struct ProPadStatus {
    bool connected = false;
    // the instrument's XInput subtype
    uint8_t subtype = 0;
    // RB3's JoypadType for the pad, which picks what reads its pro data
    uint32_t game_type = 0;
    // band3 writes this pad's pro data
    bool writing = false;
    // the last bytes written
    ProData data{};
    uint64_t writes = 0;
};

// From the hooks, each poll: `written` is the pad's new pro data, or null when
// band3 doesn't write it.
void RecordProPad(int pad, bool connected, uint8_t subtype, uint32_t game_type,
                  const ProData* written);

// false until the game has polled its pads with the hooks in place
bool ProPadsPolled();
std::array<ProPadStatus, kProPads> ProPadStatuses();

// a name for RB3's JoypadType (rb3-xenon, src/system/os/Joypad.h), for the
// Xbox types; null for the others
const char* JoypadTypeName(uint32_t type);

}
