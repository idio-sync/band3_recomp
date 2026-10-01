#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include "src/settings.h"

// The song_speed and track_speed multipliers, applied where RB3Enhanced applies
// its SongSpeedMultiplier and TrackSpeedMultiplier (source/SpeedHooks.c; the
// addresses in its include/ports_xbox360.h). The speed is each function's
// float argument, in f1.

extern "C" void __imp__Game__SetMusicSpeed(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__TrackDir__SetScrollSpeed(PPCContext& ctx, uint8_t* base);

// Game::SetMusicSpeed(Game*, float speed): 1 for a normal song. Practice mode
// and Deluxe's song speed modifier ask for other speeds, which are left alone.
extern "C" REX_FUNC(Game__SetMusicSpeed)
{
    if (ctx.f1.f64 == 1.0) ctx.f1.f64 = band3::settings::SongSpeed();
    REXLOG_DEBUG("Music speed: {:.2f}", ctx.f1.f64);
    __imp__Game__SetMusicSpeed(ctx, base);
}

// TrackDir::SetScrollSpeed(TrackDir*, float), which RB3E calls
// TrackPanelDirBase::UpdateTrackSpeed: the value is how long a gem takes to
// cross the track, so a faster track divides it
extern "C" REX_FUNC(TrackDir__SetScrollSpeed)
{
    ctx.f1.f64 /= band3::settings::TrackSpeed();
    REXLOG_DEBUG("Track scroll speed: {:.2f}", ctx.f1.f64);
    __imp__TrackDir__SetScrollSpeed(ctx, base);
}
