// For VR: the whole world in every camera shot, with vr_show_all (debug) or
// on its own in a song seen from a fixed viewpoint (vr_viewpoint front_row,
// on_stage, venue, with vr on). A viewer's head isn't the game's camera, so
// nothing may be left out for being off screen or out of the shot:
//   CamShot::DoHide hides the shot's hide lists (and shows its show list)
//     as the shot starts; skipped, nothing is hidden. UnHide then does
//     nothing too: it only undoes a DoHide that set the shot's +410 flag.
//     As a fixed viewpoint starts mid-shot, the shot that last hid is
//     undone (UnHide) at once, as its end would.
//   WorldDir::SetCrowds shows the crowds the shot lists and hides the rest
//     (a venue's audience is several WorldCrowds, and a shot usually lists
//     one, or none); after it, every crowd shows. As a fixed viewpoint
//     starts, the crowds of the WorldDir it last saw this song are shown at
//     once rather than at the next shot.
//   RndDrawable::Draw draws an object only when its bounding sphere is
//     inside the current camera's frustum; skipped, every showing object
//     draws.
// And the band's and crowds' meshes are marked as theirs in the native
// renderer's capture (scene_capture.h's MeshOwnerScope), so a fixed
// viewpoint can find the stage and the audience in any venue (vr_venue.h):
// BandCharacter::DrawLodOrShadow (music_video.cpp's hook) and
// WorldCrowd::DrawShowing.

#include <atomic>
#include <string>

#include "generated/band3_init.h"
#include "src/Hooks/vr_view.h"
#include "src/Render/native_view.h"
#include "src/Render/scene_capture.h"
#include "src/settings.h"
#include <rex/logging.h>

extern "C" void __imp__CamShot__DoHide(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__WorldDir__SetCrowds(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndDrawable__Draw(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__WorldCrowd__DrawShowing(PPCContext& ctx, uint8_t* base);
REX_EXTERN(CamShot__UnHide);

namespace {

// CamShot: the lists DoHide hides (two linked lists and a vector), the one it
// shows, and WorldDir::DrawShowing's draw overrides (a count and a list)
constexpr uint32_t kCamShot_HideList = 116;
constexpr uint32_t kCamShot_ShowList = 136;
constexpr uint32_t kCamShot_HideList2 = 156;
constexpr uint32_t kCamShot_HideVecBegin = 168;
constexpr uint32_t kCamShot_HideVecEnd = 172;
constexpr uint32_t kCamShot_DrawOverrideCount = 184;
// WorldDir: its crowds, a linked list (node +0 the crowd, +4 the next)
constexpr uint32_t kWorldDir_Crowds = 692;
// RndDrawable: its showing flag, and the vtable slot of DrawShowing
constexpr uint32_t kRndDrawable_Showing = 8;
constexpr uint32_t kRndDrawable_DrawShowing = 20;

// a fixed viewpoint in a song, as RunFrame last found it
std::atomic<bool> g_fixed_song{false};
// the WorldDir WorldDir::SetCrowds last had this song (0 outside songs:
// another song's venue may be gone)
std::atomic<uint32_t> g_world{0};
// the CamShot whose DoHide last ran this song, 0 none
std::atomic<uint32_t> g_hid_shot{0};

uint32_t ListLength(uint8_t* base, uint32_t node) {
    uint32_t n = 0;
    for (; node && n < 100000; node = REX_LOAD_U32(node + 4)) n++;
    return n;
}

bool ShowAll() { return REXCVAR_GET(vr_show_all) || g_fixed_song.load(std::memory_order_relaxed); }

uint32_t ShowCrowds(uint8_t* base, uint32_t world) {
    uint32_t shown = 0;
    for (uint32_t node = REX_LOAD_U32(world + kWorldDir_Crowds); node && shown < 1000;
         node = REX_LOAD_U32(node + 4), shown++)
        REX_STORE_U8(REX_LOAD_U32(node) + kRndDrawable_Showing, 1);
    return shown;
}

}  // namespace

namespace band3::vr_view {

void RunFrame(PPCContext& ctx, uint8_t* base) {
    const bool song = render::InSong();
    if (!song) {
        g_world.store(0, std::memory_order_relaxed);
        g_hid_shot.store(0, std::memory_order_relaxed);
    }
    bool fixed = false;
    if (song && render::SongScreen() && REXCVAR_GET(vr)) {
        const std::string mode = REXCVAR_GET(vr_viewpoint);
        fixed = mode == "front_row" || mode == "on_stage" || mode == "venue";
    }
    if (fixed == g_fixed_song.exchange(fixed, std::memory_order_relaxed)) return;
    if (!fixed) {
        REXLOG_INFO("vr: the game's shots hide what they hide again (no fixed viewpoint)");
        return;
    }
    // what this shot hid shows now, not at the next shot: the band members
    // it hid (UnHide, below the caller's frame, with its r13) and its crowds
    if (const uint32_t shot = g_hid_shot.exchange(0, std::memory_order_relaxed)) {
        PPCContext call{};
        call.r1.u64 = ctx.r1.u32 - 0x100;
        call.r13 = ctx.r13;
        call.r3.u64 = shot;
        CamShot__UnHide(call, base);
    }
    const uint32_t world = g_world.load(std::memory_order_relaxed);
    const uint32_t shown = world ? ShowCrowds(base, world) : 0;
    REXLOG_INFO("vr: a fixed viewpoint: every shot draws the whole band and all {} crowds",
                shown);
}

}  // namespace band3::vr_view

extern "C" REX_FUNC(CamShot__DoHide) {
    const uint32_t shot = ctx.r3.u32;
    if (!ShowAll()) {
        if (band3::render::InSong()) g_hid_shot.store(shot, std::memory_order_relaxed);
        __imp__CamShot__DoHide(ctx, base);
        return;
    }
    if (!REXCVAR_GET(vr_show_all)) return;
    const uint32_t hidden = ListLength(base, REX_LOAD_U32(shot + kCamShot_HideList)) +
                            ListLength(base, REX_LOAD_U32(shot + kCamShot_HideList2)) +
                            (REX_LOAD_U32(shot + kCamShot_HideVecEnd) -
                             REX_LOAD_U32(shot + kCamShot_HideVecBegin)) / 4;
    REXLOG_INFO("vr_show_all: shot {:08X} would hide {} and show {}; {} draw overrides", shot,
                hidden, ListLength(base, REX_LOAD_U32(shot + kCamShot_ShowList)),
                REX_LOAD_U32(shot + kCamShot_DrawOverrideCount));
}

extern "C" REX_FUNC(WorldDir__SetCrowds) {
    const uint32_t world = ctx.r3.u32;
    if (band3::render::InSong()) g_world.store(world, std::memory_order_relaxed);
    // the shot's crowds still get its billboard rotation
    __imp__WorldDir__SetCrowds(ctx, base);
    if (!ShowAll()) return;
    const uint32_t shown = ShowCrowds(base, world);
    if (REXCVAR_GET(vr_show_all)) REXLOG_INFO("vr_show_all: {} crowds shown", shown);
}

extern "C" REX_FUNC(RndDrawable__Draw) {
    if (!ShowAll()) {
        __imp__RndDrawable__Draw(ctx, base);
        return;
    }
    const uint32_t self = ctx.r3.u32;
    if (!REX_LOAD_U8(self + kRndDrawable_Showing)) return;
    REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(REX_LOAD_U32(self) + kRndDrawable_DrawShowing));
}

// the crowd's cards and 3D members, marked as the crowd's
extern "C" REX_FUNC(WorldCrowd__DrawShowing) {
    band3::render::MeshOwnerScope owner(band3::render::kOwnerCrowd, ctx.r3.u32);
    __imp__WorldCrowd__DrawShowing(ctx, base);
}

