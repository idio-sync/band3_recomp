#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include "generated/band3_init.h"
#include "src/settings.h"

// The skip_profile_prompt setting. When a player presses Start to join outside
// a song's setup, OvershellSlot::OnMsg(AddLocalUserResultMsg) puts their slot on
// kState_ChooseProfile: No Profile, Sign In, Swap to User. Each update,
// OvershellPanel::ResolveChooseProfileStates answers it for a signed-in player
// with LeaveOptions, the call No Profile makes (ui/overshell/slot_states.dta,
// overshell_continue_without_profile), so only players who aren't signed in see
// it. In band3 that's every player but the first, and Sign In does nothing. With
// the setting on, the join answers it for them the same way, so they join as
// guests at once. rb3-xenon, src/band3/meta_band/OvershellSlot.cpp and
// OvershellPanel.cpp.

extern "C" void __imp__OvershellSlot__OnMsg_825E17A8(PPCContext& ctx, uint8_t* base);
REX_EXTERN(OvershellSlot__GetUser);
REX_EXTERN(OvershellSlot__LeaveOptions);

namespace {

// OvershellSlotStateID
constexpr uint32_t kState_ChooseProfile = 31;
// BandUser::mOvershellState, which BandUser::SetOvershellSlotState writes
constexpr uint32_t kBandUser_OvershellState = 32;

// a stack below the hooked function's frame, with the caller's r13
PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - 0x100;
    call.r13 = ctx.r13;
    return call;
}

}

// DataNode OvershellSlot::OnMsg(this, const AddLocalUserResultMsg&): r3 is the
// DataNode it returns, r4 this
extern "C" REX_FUNC(OvershellSlot__OnMsg_825E17A8)
{
    const uint32_t slot = ctx.r4.u32;
    PPCContext call = CallContext(ctx);
    __imp__OvershellSlot__OnMsg_825E17A8(ctx, base);
    if (!REXCVAR_GET(skip_profile_prompt)) return;

    call.r3.u64 = slot;
    OvershellSlot__GetUser(call, base);
    const uint32_t user = call.r3.u32;
    if (!user || REX_LOAD_U32(user + kBandUser_OvershellState) != kState_ChooseProfile) return;

    REXLOG_INFO("skip_profile_prompt: a player joined; answering Choose Profile with No Profile");
    call = CallContext(ctx);
    call.r3.u64 = slot;
    OvershellSlot__LeaveOptions(call, base);
}
