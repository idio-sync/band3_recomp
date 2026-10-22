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
//   BandCamShot::StartAnim hides the band members the shot lists with mHide
//     until it ends (a drum close-up hides the rest of the band); its hook
//     (graphics.cpp's, force_self_shadow's too) clears their flag while it
//     reads them (ShotMayHideBand).
// And the band's and crowds' meshes are marked as theirs in the native
// renderer's capture (scene_capture.h's MeshOwnerScope), so a fixed
// viewpoint can find the stage and the audience in any venue (vr_venue.h):
// BandCharacter::DrawLodOrShadow (music_video.cpp's hook) and
// WorldCrowd::DrawShowing.
// With vr_hold_band too, the band stays where it is across the game's cuts
// (vr_teleport.h): BandCamShot::TeleportTarget, which moves each band member
// onto the shot's mark as it starts and back as it ends, is skipped, and so
// is BandCharacter::Poll's move home at every shot; a leash keeps each member
// near its place instead.

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <mutex>
#include <string>
#include <vector>

#include "generated/band3_init.h"
#include "src/Hooks/vr_teleport.h"
#include "src/Hooks/vr_view.h"
#include "src/Render/native_view.h"
#include "src/Render/scene_capture.h"
#include "src/settings.h"
#include <rex/logging.h>

extern "C" void __imp__CamShot__DoHide(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__WorldDir__SetCrowds(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndDrawable__Draw(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__WorldCrowd__DrawShowing(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BandCamShot__TeleportTarget(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__BandCharacter__Poll(PPCContext& ctx, uint8_t* base);
REX_EXTERN(CamShot__UnHide);
REX_EXTERN(RndTransformable__SetDirty_Force);

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

// RndTransformable: its local transform (Matrix3 rows, then the translation,
// 16 bytes each), which TeleportTarget sets
constexpr uint32_t kTrans_LocalXfm = 28;
constexpr uint32_t kTrans_LocalPos = kTrans_LocalXfm + 48;
// and its world transform, and whether that's stale (WorldXfm would make it
// again; SetDirty is skipped while it's set)
constexpr uint32_t kTrans_WorldXfm = 92;
constexpr uint32_t kTrans_Dirty = 156;
// Character: its RndTransformable (TeleportTarget's target is this), its
// RndPollable (BandCharacter::Poll's `this`), and BandCharacter's waypoint (where
// the venue puts it on the stage, which Teleport() sends it to); and the
// Pollable's go-home flag (BandWardrobe::StartVenueShot's)
constexpr uint32_t kCharacter_Trans = 212;
constexpr uint32_t kCharacter_Pollable = 392;
constexpr uint32_t kBandCharacter_Waypoint = 1528;
constexpr uint32_t kPoll_GoHome = 1142;

// a guest heap object's address, not a stray value (RB3's objects live
// from 0x10000000 up)
bool HeapAddress(uint32_t a) { return a >= 0x10000000u && a < 0xF0000000u; }

// A band member's move between two frames that reads as a jump from a fixed
// viewpoint: walking is a few centimetres a frame, a shot's mark a metre or more
// away and often turned tens of degrees
constexpr float kJumpUnits = 0.25f * 39.37f;
constexpr float kJumpDegrees = 30;

// vr_hold_band this song: the shots' teleports let through or not
// (TeleportGate). And for the log, from a fixed viewpoint (whether held or
// not): each band member polled from it this song (BandCharacter::Poll; by
// its RndTransformable), where it stood the frame before, the jumps it made
// between two frames (more than kJumpUnits or kJumpDegrees), how far it got
// from its waypoint, how many teleports were skipped and how often the leash
// pulled
struct Member {
    uint32_t target = 0;
    bool seen = false;          // `last` is this stretch's
    float last[3] = {0, 0, 0};  // x, y, heading in degrees
    float farthest = 0, turned = 0;  // from its waypoint: venue units, degrees
    // and summed over the frames it had one, for the means
    double distance_sum = 0, turn_sum = 0;
    uint32_t homed_frames = 0;
    // the leash's last look (Leash), 0 none yet
    int64_t leashed_ns = 0;
};
struct Hold {
    std::mutex mutex;
    band3::vr_view::TeleportGate gate;
    std::vector<Member> members;
    uint32_t skipped = 0, jumps = 0, frames = 0, pulled = 0;
    // members a shot's StartAnim would have hidden (BandKeptShowing)
    uint32_t unhid = 0;
    // the game thread's time in WatchBand and Leash
    int64_t spent_ns = 0;
    float biggest = 0;  // the biggest jump's distance, venue units
    bool song = false;
};
Hold g_hold;
constexpr size_t kMaxMembers = 16;

float LoadFloat(uint8_t* base, uint32_t addr) { return std::bit_cast<float>(REX_LOAD_U32(addr)); }

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// adds the time since `from` to the hold's own (for the log's line)
void Spent(int64_t from) {
    const int64_t took = NowNs() - from;
    std::lock_guard lock(g_hold.mutex);
    g_hold.spent_ns += took;
}

// heading in degrees, counterclockwise, of a transform's y row (its forward)
float HeadingOf(uint8_t* base, uint32_t xfm) {
    return std::atan2(-LoadFloat(base, xfm + 16), LoadFloat(base, xfm + 20)) * 57.2957795f;
}

// a target's place across the floor and heading, from its local transform
void PlaceOf(uint8_t* base, uint32_t t, float out[3]) {
    out[0] = LoadFloat(base, t + kTrans_LocalPos);
    out[1] = LoadFloat(base, t + kTrans_LocalPos + 4);
    out[2] = HeadingOf(base, t + kTrans_LocalXfm);
}

// where Teleport() would put member `t`: its waypoint's world transform (which
// Character::Teleport copies into the character's local one); false without
// one, or while it's stale
bool HomeOf(uint8_t* base, uint32_t t, float out[3]) {
    const uint32_t way = REX_LOAD_U32(t - kCharacter_Trans + kBandCharacter_Waypoint);
    if (!HeapAddress(way) || REX_LOAD_U8(way + kTrans_Dirty)) return false;
    out[0] = LoadFloat(base, way + kTrans_WorldXfm + 48);
    out[1] = LoadFloat(base, way + kTrans_WorldXfm + 52);
    out[2] = HeadingOf(base, way + kTrans_WorldXfm);
    return std::isfinite(out[0] + out[1] + out[2]);
}

// `t`'s Member, added if there's room; null if not (g_hold.mutex held)
Member* MemberOf(uint32_t t) {
    for (Member& m : g_hold.members)
        if (m.target == t) return &m;
    if (g_hold.members.size() >= kMaxMembers) return nullptr;
    g_hold.members.push_back({t});
    return &g_hold.members.back();
}

// one frame from a fixed viewpoint: each member's move since the last, and
// how far it is from its waypoint
void WatchBand(uint8_t* base) {
    std::lock_guard lock(g_hold.mutex);
    g_hold.frames++;
    for (Member& m : g_hold.members) {
        float now[3], home[3];
        PlaceOf(base, m.target, now);
        if (m.seen) {
            const float moved = std::hypot(now[0] - m.last[0], now[1] - m.last[1]);
            const float turned = std::fabs(std::remainder(now[2] - m.last[2], 360.0f));
            if (moved > kJumpUnits || turned > kJumpDegrees) {
                g_hold.jumps++;
                g_hold.biggest = std::max(g_hold.biggest, moved);
            }
        }
        if (HomeOf(base, m.target, home)) {
            const float away = std::hypot(now[0] - home[0], now[1] - home[1]);
            const float turned = std::fabs(std::remainder(now[2] - home[2], 360.0f));
            m.farthest = std::max(m.farthest, away);
            m.turned = std::max(m.turned, turned);
            m.distance_sum += away;
            m.turn_sum += turned;
            m.homed_frames++;
        }
        m.seen = true;
        std::copy(now, now + 3, m.last);
    }
}

// The held member whose Character is `character` pulled toward its waypoint
// (LeashStep): its local transform's place and heading moved (the heading
// turned about its feet) and marked dirty as Teleport() does
void Leash(PPCContext& ctx, uint8_t* base, uint32_t character) {
    const uint32_t t = character + kCharacter_Trans;
    float home[3], now[3];
    if (!HomeOf(base, t, home)) return;
    PlaceOf(base, t, now);
    if (!std::isfinite(now[0] + now[1] + now[2])) return;
    const int64_t ns = NowNs();
    float dt = 0;
    {
        std::lock_guard lock(g_hold.mutex);
        Member* m = MemberOf(t);
        if (!m) return;
        // a frame's time, at most a tenth of a second (after a pause, a
        // loading screen)
        if (m->leashed_ns) dt = std::min(0.1f, float(ns - m->leashed_ns) / 1e9f);
        m->leashed_ns = ns;
    }
    const float was[3] = {now[0], now[1], now[2]};
    if (!band3::vr_view::LeashStep(home, now, dt, 39.37f)) return;
    {
        std::lock_guard lock(g_hold.mutex);
        g_hold.pulled++;
    }
    REX_STORE_U32(t + kTrans_LocalPos, std::bit_cast<uint32_t>(now[0]));
    REX_STORE_U32(t + kTrans_LocalPos + 4, std::bit_cast<uint32_t>(now[1]));
    // the rows (x, y, z axes) turned about z by the heading's change
    const float turn = (now[2] - was[2]) * 0.0174532925f;
    const float c = std::cos(turn), s = std::sin(turn);
    for (uint32_t row = 0; row < 3; row++) {
        const uint32_t at = t + kTrans_LocalXfm + row * 16;
        const float x = LoadFloat(base, at), y = LoadFloat(base, at + 4);
        REX_STORE_U32(at, std::bit_cast<uint32_t>(x * c - y * s));
        REX_STORE_U32(at + 4, std::bit_cast<uint32_t>(x * s + y * c));
    }
    if (!REX_LOAD_U8(t + kTrans_Dirty)) {
        PPCContext call{};
        call.r1.u64 = ctx.r1.u32 - 0x100;
        call.r13 = ctx.r13;
        call.r3.u64 = t;
        RndTransformable__SetDirty_Force(call, base);
    }
}

// a fixed viewpoint's stretch of a song is over: what the band did in it,
// logged, and forgotten (and with `song_over`, the members the shots moved)
void EndStretch(bool song_over) {
    std::lock_guard lock(g_hold.mutex);
    if (g_hold.frames && !g_hold.members.empty()) {
        std::string walked;
        for (const Member& m : g_hold.members) {
            if (!m.homed_frames) continue;
            walked += fmt::format("{}{:.2f} m, {:.0f} degrees (on average {:.2f} m, {:.0f})",
                                  walked.empty() ? "" : "; ", m.farthest / 39.37f, m.turned,
                                  m.distance_sum / m.homed_frames / 39.37,
                                  m.turn_sum / m.homed_frames);
        }
        REXLOG_INFO("vr: from the fixed viewpoint, over {} frames, the band jumped {} times (a "
                    "member moving more than {} m or turning more than {} degrees in a frame; "
                    "the biggest {:.2f} m); vr_hold_band skipped {} shots' moves onto their "
                    "marks or back and pulled a member toward its place {} times; {} members "
                    "the shots would have hidden were kept showing; the farthest each member "
                    "got from its place on the stage (its waypoint): {}; watching and leashing "
                    "the band took the game's thread {:.1f} us a frame",
                    g_hold.frames, g_hold.jumps, kJumpUnits / 39.37f, kJumpDegrees,
                    g_hold.biggest / 39.37f, g_hold.skipped, g_hold.pulled, g_hold.unhid, walked,
                    double(g_hold.spent_ns) / 1e3 / g_hold.frames);
    }
    g_hold.frames = g_hold.jumps = g_hold.skipped = g_hold.pulled = g_hold.unhid = 0;
    g_hold.spent_ns = 0;
    g_hold.biggest = 0;
    for (Member& m : g_hold.members) m = {m.target};
    if (!song_over) return;
    g_hold.gate.Reset();
    g_hold.members.clear();
}

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

bool ShotMayHideBand() { return !g_fixed_song.load(std::memory_order_relaxed); }

void BandKeptShowing(uint32_t members) {
    std::lock_guard lock(g_hold.mutex);
    g_hold.unhid += members;
}

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
    const bool was_fixed = g_fixed_song.exchange(fixed, std::memory_order_relaxed);
    bool song_over = false;
    {
        std::lock_guard lock(g_hold.mutex);
        song_over = g_hold.song && !song;
        g_hold.song = song;
    }
    if (was_fixed && !fixed) EndStretch(song_over);
    else if (song_over) EndStretch(true);
    if (fixed) {
        const int64_t from = NowNs();
        WatchBand(base);
        Spent(from);
    }
    if (fixed == was_fixed) return;
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
    REXLOG_INFO("vr: a fixed viewpoint: every shot draws the whole band and all {} crowds{}",
                shown,
                REXCVAR_GET(vr_hold_band) ? ", and the band stays off the shots' marks" : "");
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

// BandCamShot::TeleportTarget(this, RndTransformable* t, const TransformNoScale&
// to, bool back): t's local transform set to `to` and, for a character, its
// "teleport_char" message. Held (vr_hold_band from a fixed viewpoint in a
// song), the shots' moves of the band onto their marks and back are skipped
// (vr_teleport.h's TeleportGate).
extern "C" REX_FUNC(BandCamShot__TeleportTarget) {
    const uint32_t t = ctx.r4.u32;
    const bool back = (ctx.r6.u32 & 0xff) != 0;
    const bool hold = g_fixed_song.load(std::memory_order_relaxed) && REXCVAR_GET(vr_hold_band);
    {
        std::lock_guard lock(g_hold.mutex);
        if (!g_hold.gate.Allow(t, back, hold)) {
            g_hold.skipped++;
            return;
        }
    }
    __imp__BandCamShot__TeleportTarget(ctx, base);
}

// BandCharacter::Poll (its RndPollable, 392 bytes into the Character): first
// thing, with its go-home flag set (BandWardrobe::StartVenueShot sets it at
// every shot), Teleport() onto its waypoint. Held, the flag is cleared, and
// the leash (vr_teleport.h's LeashStep) keeps the member near its waypoint.
extern "C" REX_FUNC(BandCharacter__Poll) {
    const uint32_t poll = ctx.r3.u32;
    // the band members watched from a fixed viewpoint: those polled in a
    // song (a shot's targets can be other characters, whose layout isn't a
    // BandCharacter's)
    if (g_fixed_song.load(std::memory_order_relaxed)) {
        std::lock_guard lock(g_hold.mutex);
        MemberOf(poll - kCharacter_Pollable + kCharacter_Trans);
    }
    if (g_fixed_song.load(std::memory_order_relaxed) && REXCVAR_GET(vr_hold_band)) {
        REX_STORE_U8(poll + kPoll_GoHome, 0);
        const int64_t from = NowNs();
        Leash(ctx, base, poll - kCharacter_Pollable);
        Spent(from);
    }
    __imp__BandCharacter__Poll(ctx, base);
}

// the crowd's cards and 3D members, marked as the crowd's
extern "C" REX_FUNC(WorldCrowd__DrawShowing) {
    band3::render::MeshOwnerScope owner(band3::render::kOwnerCrowd, ctx.r3.u32);
    __imp__WorldCrowd__DrawShowing(ctx, base);
}

