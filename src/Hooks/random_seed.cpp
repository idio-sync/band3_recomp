#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/settings.h"

// test_random_seed. RB3 seeds its random numbers (gRand, behind RandomInt,
// RandomFloat and the scripts' random functions) once, in SystemPreInit, with
// the milliseconds of the local time, so what it picks at random differs from
// run to run: on a fresh profile, the band it makes up and their outfits. With
// the setting, that seed is the setting's, so a fresh profile gets the same
// band on every launch, for render checks that look at particular characters.
// It doesn't make a song's frames repeat exactly: particles, the crowd and the
// characters' animations draw from gRand every frame, as many times as the
// frame's timing gives, so poses (and the camera angles that follow them)
// still vary; the shot categories come from the song's venue track.
// rb3-xenon, src/system/math/Rand.cpp and src/system/os/System.cpp.

extern "C" void __imp__SeedRand(PPCContext& ctx, uint8_t* base);

namespace {

// where SystemPreInit's SeedRand call returns to; a script's random_seed is
// the only other caller, and is left alone
constexpr uint32_t kBootSeedReturn = 0x82511030;

}

// void SeedRand(int seed)
extern "C" REX_FUNC(SeedRand)
{
    const int32_t seed = REXCVAR_GET(test_random_seed);
    if (seed != 0 && ctx.lr == kBootSeedReturn) {
        REXLOG_INFO("test_random_seed: seeding RB3's random numbers with {} instead of {}",
                    seed, ctx.r3.s32);
        ctx.r3.s64 = seed;
    }
    __imp__SeedRand(ctx, base);
}
