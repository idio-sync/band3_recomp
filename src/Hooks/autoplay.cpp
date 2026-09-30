#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/settings.h"

// The autoplay setting. When a song starts, GemPlayer::Start (guitar, bass,
// drums, keys, pro guitar) and VocalPlayer::Start set each player's autoplay
// from its BandUser, which only the game's debug menus change. With the setting
// on, every player is switched to autoplay right after, through the same
// SetAutoplay the debug menus use (rb3-xenon, src/band3/game/GemPlayer.cpp and
// VocalPlayer.cpp).

extern "C" void __imp__GemPlayer__Start(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__VocalPlayer__Start(PPCContext& ctx, uint8_t* base);
REX_EXTERN(GemPlayer__SetAutoplay);
REX_EXTERN(VocalPlayer__SetAutoplay);

namespace {

// a stack below the hooked function's frame, with the caller's r13
PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

}

extern "C" REX_FUNC(GemPlayer__Start)
{
    const uint32_t player = ctx.r3.u32;
    __imp__GemPlayer__Start(ctx, base);
    if (!REXCVAR_GET(autoplay)) return;
    PPCContext call = CallContext(ctx);
    call.r3.u64 = player;
    call.r4.u64 = 1;
    GemPlayer__SetAutoplay(call, base);
}

extern "C" REX_FUNC(VocalPlayer__Start)
{
    const uint32_t player = ctx.r3.u32;
    __imp__VocalPlayer__Start(ctx, base);
    if (!REXCVAR_GET(autoplay)) return;
    PPCContext call = CallContext(ctx);
    call.r3.u64 = player;
    call.r4.u64 = 1;
    VocalPlayer__SetAutoplay(call, base);
}
