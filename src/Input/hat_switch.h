#pragma once
#include <cstdint>
#include "instruments.h"

// The d-pad as PlayStation-style HID reports send it: a hat switch value, 0 for
// up and clockwise to 7 for up-left; anything else (8 on PS3/PS4, 15 on PS5)
// is centered.

namespace band3::input {

struct HatSwitch {
    bool up = false;
    bool right = false;
    bool down = false;
    bool left = false;
};

inline HatSwitch ReadHatSwitch(uint8_t value) {
    const uint8_t hat = value & 0x0F;
    return {hat == 7 || hat == 0 || hat == 1, hat >= 1 && hat <= 3, hat >= 3 && hat <= 5,
            hat >= 5 && hat <= 7};
}

inline uint16_t DpadButtons(const HatSwitch& hat) {
    return (hat.up ? xbox::kDpadUp : 0) | (hat.down ? xbox::kDpadDown : 0) |
           (hat.left ? xbox::kDpadLeft : 0) | (hat.right ? xbox::kDpadRight : 0);
}

}
