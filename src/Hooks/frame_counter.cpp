#include <rex/dbg.h>
#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/Test/game_state.h"

// Counts the frames RB3 draws, for the test harness's `wait frames=N`.
// App::DrawRegular runs once per frame; profiling builds also give it its zone
// here, next to the others in profile_zones.cpp.

extern "C" void __imp__App__DrawRegular(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(App__DrawRegular)
{
#ifdef REXGLUE_ENABLE_PROFILING
    SCOPE_profile_cpu_f("RB3 App::DrawRegular");
#endif
    band3::test::GameState::Get().CountFrame();
    __imp__App__DrawRegular(ctx, base);
}
