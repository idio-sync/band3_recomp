#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include "generated/band3_init.h"
#include "src/Input/input_lock.h"

// Serializes the game's calls into the SDK's input system (see input_lock.h).
// These are the only guest functions that call XamInputGetState,
// XamInputGetCapabilities or XamInputSetState: the XInput library's wrappers.

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

// XInputGetState: XamInputGetState(user, 1, state)
BAND3_INPUT_LOCKED(rex_sub_8283FB80)
// XInputGetCapabilities: XamInputGetCapabilities
BAND3_INPUT_LOCKED(sub_8283FB78)
// XInput2's device capabilities read
BAND3_INPUT_LOCKED(rex_sub_8284E1D8)
// XInput2's poll, which takes the library's own critical section and calls
// XamInputGetState and the capabilities read above
BAND3_INPUT_LOCKED(XInput2Sample)
// XamInputGetCapabilities, then XamInputSetState (vibration)
BAND3_INPUT_LOCKED(rb3_XInputSetState)
