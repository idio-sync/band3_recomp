// Experimental, for VR: vr_show_all draws the whole world in every camera
// shot. A viewer's head isn't the game's camera, so nothing may be left out
// for being off screen or out of the shot:
//   CamShot::DoHide hides the shot's hide lists (and shows its show list)
//     as the shot starts; skipped, nothing is hidden. UnHide then does
//     nothing too: it only undoes a DoHide that set the shot's +410 flag.
//   RndDrawable::Draw draws an object only when its bounding sphere is
//     inside the current camera's frustum; skipped, every showing object
//     draws.

#include "generated/band3_init.h"
#include "src/settings.h"
#include <rex/logging.h>

extern "C" void __imp__CamShot__DoHide(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndDrawable__Draw(PPCContext& ctx, uint8_t* base);

namespace {

// CamShot: the lists DoHide hides (two linked lists and a vector), the one it
// shows, and WorldDir::DrawShowing's draw overrides (a count and a list)
constexpr uint32_t kCamShot_HideList = 116;
constexpr uint32_t kCamShot_ShowList = 136;
constexpr uint32_t kCamShot_HideList2 = 156;
constexpr uint32_t kCamShot_HideVecBegin = 168;
constexpr uint32_t kCamShot_HideVecEnd = 172;
constexpr uint32_t kCamShot_DrawOverrideCount = 184;
// RndDrawable: its showing flag, and the vtable slot of DrawShowing
constexpr uint32_t kRndDrawable_Showing = 8;
constexpr uint32_t kRndDrawable_DrawShowing = 20;

uint32_t ListLength(uint8_t* base, uint32_t node) {
    uint32_t n = 0;
    for (; node && n < 100000; node = REX_LOAD_U32(node + 4)) n++;
    return n;
}

}  // namespace

extern "C" REX_FUNC(CamShot__DoHide) {
    if (!REXCVAR_GET(vr_show_all)) {
        __imp__CamShot__DoHide(ctx, base);
        return;
    }
    const uint32_t shot = ctx.r3.u32;
    const uint32_t hidden = ListLength(base, REX_LOAD_U32(shot + kCamShot_HideList)) +
                            ListLength(base, REX_LOAD_U32(shot + kCamShot_HideList2)) +
                            (REX_LOAD_U32(shot + kCamShot_HideVecEnd) -
                             REX_LOAD_U32(shot + kCamShot_HideVecBegin)) / 4;
    REXLOG_INFO("vr_show_all: shot {:08X} would hide {} and show {}; {} draw overrides", shot,
                hidden, ListLength(base, REX_LOAD_U32(shot + kCamShot_ShowList)),
                REX_LOAD_U32(shot + kCamShot_DrawOverrideCount));
}

extern "C" REX_FUNC(RndDrawable__Draw) {
    if (!REXCVAR_GET(vr_show_all)) {
        __imp__RndDrawable__Draw(ctx, base);
        return;
    }
    const uint32_t self = ctx.r3.u32;
    if (!REX_LOAD_U8(self + kRndDrawable_Showing)) return;
    REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(REX_LOAD_U32(self) + kRndDrawable_DrawShowing));
}
