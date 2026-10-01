#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include <mutex>
#include <random>
#include "generated/band3_init.h"

// XNetRandom(buffer, length), which RB3 reaches only through this wrapper. The
// SDK's NetDll_XNetRandom fills the buffer with 0xBB, so every HxGuid RB3
// generates is the same, and every local player has player 1's user guid: the
// track config, keyed by it, can't find a second player's track, and a song
// with more than one player crashes as it loads (GemManager asks SongData for
// track -1). Real random bytes, as the console gives, keep the guids apart.

extern "C" REX_FUNC(sub_8284D800)
{
    const uint32_t buffer = ctx.r3.u32;
    const uint32_t length = ctx.r4.u32;
    static std::mutex mutex;
    static std::mt19937_64 random{std::random_device{}()};
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (uint32_t i = 0; i < length; i++) {
            REX_STORE_U8(buffer + i, static_cast<uint8_t>(random()));
        }
    }
    ctx.r3.u64 = 0;
}
