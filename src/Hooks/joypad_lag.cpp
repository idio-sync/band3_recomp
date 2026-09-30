#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/Input/joypad_lag_status.h"
#include "src/settings.h"

// The extra lag RB3 builds in per controller type (see
// Input/joypad_lag_status.h). ProfileMgr's constructor asks
// GetJoypadExtraLagInits for every JoypadType and LagContext once and keeps the
// answers; this hands back joypad_lag's number where it sets one, and notes
// both for the Instrument Lab.

extern "C" void __imp__ProfileMgr__GetJoypadExtraLagInits(PPCContext& ctx, uint8_t* base);

namespace {

using namespace band3::input;

// joypad_lag, read the first time the game asks, which is at startup
const JoypadLagOverrides& Overrides() {
    static const JoypadLagOverrides overrides = [] {
        JoypadLagOverrides o{};
        for (const auto& bad : ParseJoypadLagOverrides(REXCVAR_GET(joypad_lag), o)) {
            REXLOG_WARN("joypad_lag: can't read '{}' (type=ms or type=ms/video/audio)", bad);
        }
        return o;
    }();
    return overrides;
}

}

// float ProfileMgr::GetJoypadExtraLagInits(this, JoypadType, LagContext) const
extern "C" REX_FUNC(ProfileMgr__GetJoypadExtraLagInits)
{
    const uint32_t type = ctx.r4.u32;
    const uint32_t context = ctx.r5.u32;
    __imp__ProfileMgr__GetJoypadExtraLagInits(ctx, base);

    const float game_ms = static_cast<float>(ctx.f1.f64);
    const float used_ms = ApplyJoypadLagOverride(Overrides(), type, context, game_ms);
    if (used_ms != game_ms) {
        REXLOG_INFO("joypad_lag: type {} {} lag {} ms (the game's is {} ms)", type,
                    LagContextName(static_cast<int>(context)), used_ms, game_ms);
        ctx.f1.f64 = used_ms;
    }
    RecordJoypadLag(type, context, game_ms, used_ms);
}
