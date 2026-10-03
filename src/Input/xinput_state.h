#pragma once
#include <cstring>
#include <rex/input/input.h>
#include "instruments.h"

// Copies band3's host-order instrument state to and from the guest's big-endian
// XInput structs.

namespace band3::input {

inline void StoreGamepad(const Gamepad360& g, rex::input::X_INPUT_GAMEPAD& out) {
    out.buttons = g.buttons;
    out.left_trigger = g.left_trigger;
    out.right_trigger = g.right_trigger;
    out.thumb_lx = g.thumb_lx;
    out.thumb_ly = g.thumb_ly;
    out.thumb_rx = g.thumb_rx;
    out.thumb_ry = g.thumb_ry;
}

inline Gamepad360 LoadGamepad(const rex::input::X_INPUT_GAMEPAD& in) {
    Gamepad360 g;
    g.buttons = in.buttons;
    g.left_trigger = in.left_trigger;
    g.right_trigger = in.right_trigger;
    g.thumb_lx = in.thumb_lx;
    g.thumb_ly = in.thumb_ly;
    g.thumb_rx = in.thumb_rx;
    g.thumb_ry = in.thumb_ry;
    return g;
}

inline void StoreCaps(const Caps360& caps, rex::input::X_INPUT_CAPABILITIES& out) {
    std::memset(&out, 0, sizeof(out));
    out.type = 0x01;  // XINPUT_DEVTYPE_GAMEPAD
    out.sub_type = caps.sub_type;
    out.flags = caps.flags;
    StoreGamepad(caps.gamepad, out.gamepad);
}

inline Caps360 LoadCaps(const rex::input::X_INPUT_CAPABILITIES& in) {
    Caps360 caps;
    caps.sub_type = in.sub_type;
    caps.flags = in.flags;
    caps.gamepad = LoadGamepad(in.gamepad);
    return caps;
}

}
