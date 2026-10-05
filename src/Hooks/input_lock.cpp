#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include <optional>
#include "generated/band3_init.h"
#include "src/Input/input_lock.h"
#include "src/Input/menu_shortcut.h"

// Serializes the game's calls into the SDK's input system (see input_lock.h).
// These are the only guest functions that call XamInputGetState,
// XamInputGetCapabilities or XamInputSetState: the XInput library's wrappers.
// Each player's state and type, as the game reads them, also go to the menu
// shortcut (menu_shortcut.h).

namespace band3::input {

std::recursive_mutex& InputLock() {
    static std::recursive_mutex lock;
    return lock;
}

}

#define BAND3_INPUT_LOCKED(function)                                           \
    extern "C" void __imp__##function(PPCContext& ctx, uint8_t* base);         \
    extern "C" REX_FUNC(function)                                              \
    {                                                                          \
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock()); \
        __imp__##function(ctx, base);                                          \
    }

// XInputGetState(user, state): XamInputGetState(user, 1, state). The game's
// joypad loop (RunXinputJoypadLoop) reads every player through it each pass.
extern "C" void __imp__rex_sub_8283FB80(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(rex_sub_8283FB80) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t state = ctx.r4.u32;
    {
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock());
        __imp__rex_sub_8283FB80(ctx, base);
    }
    std::optional<uint16_t> buttons;
    if (ctx.r3.u32 == 0 && state) {
        buttons = reinterpret_cast<const rex::input::X_INPUT_STATE*>(base + state)->gamepad.buttons;
    }
    band3::input::GameChordPads().OnState(user, buttons, band3::input::ChordPads::Clock::now());
}

// XInputGetCapabilities(user, flags, caps): XamInputGetCapabilities. The game
// reads a player's type through it when the player connects.
extern "C" void __imp__sub_8283FB78(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(sub_8283FB78) {
    const uint32_t user = ctx.r3.u32;
    const uint32_t caps = ctx.r5.u32;
    {
        std::lock_guard<std::recursive_mutex> lock(band3::input::InputLock());
        __imp__sub_8283FB78(ctx, base);
    }
    if (ctx.r3.u32 == 0 && caps) {
        band3::input::GameChordPads().OnCapabilities(
            user, reinterpret_cast<const rex::input::X_INPUT_CAPABILITIES*>(base + caps)->sub_type);
    }
}

// XInput2's device capabilities read
BAND3_INPUT_LOCKED(rex_sub_8284E1D8)
// XInput2's poll, which takes the library's own critical section and calls
// XamInputGetState and the capabilities read above
BAND3_INPUT_LOCKED(XInput2Sample)
// XamInputGetCapabilities, then XamInputSetState (vibration)
BAND3_INPUT_LOCKED(rb3_XInputSetState)
