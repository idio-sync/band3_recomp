#include <rex/hook.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/Input/joypad_lag_status.h"

// Records the extra lag RB3 builds in per controller type (see
// Input/joypad_lag_status.h). ProfileMgr's constructor asks
// GetJoypadExtraLagInits for every JoypadType and LagContext once and keeps the
// answers; this passes each answer through unchanged and notes it for the
// Instrument Lab.

extern "C" void __imp__ProfileMgr__GetJoypadExtraLagInits(PPCContext& ctx, uint8_t* base);

// float ProfileMgr::GetJoypadExtraLagInits(this, JoypadType, LagContext) const
extern "C" REX_FUNC(ProfileMgr__GetJoypadExtraLagInits)
{
    const uint32_t type = ctx.r4.u32;
    const uint32_t context = ctx.r5.u32;
    __imp__ProfileMgr__GetJoypadExtraLagInits(ctx, base);
    band3::input::RecordJoypadLag(type, context, static_cast<float>(ctx.f1.f64));
}
