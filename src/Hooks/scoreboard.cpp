#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "src/Test/game_state.h"

// Follows the band's score as the song's scoreboard shows it, for the test
// harness's `wait score>=N` (a part sung through a USB mic scoring, say).
// BandScoreboard::SetScore(this, int score) ignores a negative score, as this
// does.

extern "C" void __imp__BandScoreboard__SetScore(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(BandScoreboard__SetScore)
{
    if (ctx.r4.s32 >= 0) band3::test::GameState::Get().SetScore(ctx.r4.s32);
    __imp__BandScoreboard__SetScore(ctx, base);
}
