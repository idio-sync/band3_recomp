#include "src/Hooks/world_poll.h"

#include <rex/dbg.h>
#include <rex/hook.h>
#include <algorithm>
#include <bit>
#include <cstdint>
#include <unordered_map>
#include "generated/band3_init.h"
#include "src/Hooks/frame_pacing.h"

namespace {

// TaskMgr::DeltaSeconds: its timeline's seconds less the previous frame's
constexpr uint32_t kTheTaskMgr = 0x82E051A0;
constexpr uint32_t kTaskMgr_Timeline = 40;
constexpr uint32_t kTimeline_Seconds = 16;
constexpr uint32_t kTimeline_LastSeconds = 20;
// TheRnd's proc_cmds for the frame (scene_capture.cpp's kRnd_ProcCmds)
constexpr uint32_t kTheRndHolder = 0x82C76B68;
constexpr uint32_t kRnd_ProcCmds = 0x16c;
// WorldDir::Poll polls (0x824CCA38): with this global set always (another
// path, by RndDir::Poll), else when the dir's flag at +504 asks for it (the
// poll clears it) or the frame isn't world-only
constexpr uint32_t kWorldPollAlways = 0x82CC7738;
constexpr uint32_t kWorldDir_ForcePoll = 504;
constexpr uint32_t kProcWorldOnly = 1;

// frames are counted to this at most (a dir not polled for long): a world
// polled every 48 frames (background_fps 5 at 240 Hz) still counts them all.
// A stall is one frame, however long.
constexpr uint32_t kMaxFrames = 64;

float LoadFloat(uint8_t* base, uint32_t addr) {
    return std::bit_cast<float>(REX_LOAD_U32(addr));
}

// the game thread's: each dir's frames since its last poll, and the poll's
// running now (nested dirs, polled by an outer dir's poll, share its count)
thread_local std::unordered_map<uint32_t, uint32_t> t_frames;
thread_local uint32_t t_polling_frames = 0;
thread_local int t_depth = 0;

}  // namespace

namespace band3::world_poll {

uint32_t FramesSincePoll() { return t_polling_frames; }

}  // namespace band3::world_poll

extern "C" void __imp__WorldDir__Poll(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(WorldDir__Poll)
{
#ifdef REXGLUE_ENABLE_PROFILING
    SCOPE_profile_cpu_f("RB3 WorldDir::Poll");
#endif
    if (t_depth > 0) {
        t_depth++;
        __imp__WorldDir__Poll(ctx, base);
        t_depth--;
        return;
    }
    const uint32_t dir = ctx.r3.u32;
    const uint32_t timeline = REX_LOAD_U32(kTheTaskMgr + kTaskMgr_Timeline);
    const bool moved = timeline && LoadFloat(base, timeline + kTimeline_Seconds) !=
                                       LoadFloat(base, timeline + kTimeline_LastSeconds);
    if (t_frames.size() > 64) t_frames.clear();  // dirs come and go with venues
    uint32_t& frames = t_frames[dir];
    if (moved) frames = std::min(frames + 1, kMaxFrames);

    const uint32_t rnd = REX_LOAD_U32(kTheRndHolder);
    const bool polls = REX_LOAD_U32(kWorldPollAlways) != 0 ||
                       REX_LOAD_U8(dir + kWorldDir_ForcePoll) != 0 || !rnd ||
                       REX_LOAD_U32(rnd + kRnd_ProcCmds) != kProcWorldOnly;
    if (!polls) {
        __imp__WorldDir__Poll(ctx, base);
        return;
    }
    t_polling_frames = frames;
    frames = 0;
    t_depth = 1;
    __imp__WorldDir__Poll(ctx, base);
    t_depth = 0;
    t_polling_frames = 0;
}

// CharHair::Poll steps its hair once a poll, as if a poll were 1/GetFPS
// seconds: the venue's emulate_fps (when the hair follows it, +69) as the
// game turns it into a rate. Polled more often than that (background_fps
// above the venue's), the hair swung that much faster: the rate is scaled to
// how often the world really polls, which leaves it as it was at the venue's
// own rate.
namespace {
constexpr uint32_t kCurrentPostProc = 0x82CC27F0;  // RndPostProc::Current's
constexpr uint32_t kPostProc_EmulateFps = 0x168;
constexpr uint32_t kCharHair_FollowsPost = 69;
}  // namespace

extern "C" void __imp__CharHair__GetFPS(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(CharHair__GetFPS)
{
    const uint32_t hair = ctx.r3.u32;
    __imp__CharHair__GetFPS(ctx, base);
    const uint32_t frames = band3::world_poll::FramesSincePoll();
    if (!frames || !REX_LOAD_U8(hair + kCharHair_FollowsPost)) return;
    const uint32_t pp = REX_LOAD_U32(kCurrentPostProc);
    if (!pp) return;
    const float emulate = std::bit_cast<float>(REX_LOAD_U32(pp + kPostProc_EmulateFps));
    if (!(emulate > 0)) return;
    const double hz = band3::pacing::GameHz() > 0 ? band3::pacing::GameHz() : 60.0;
    const float scale = band3::world_poll::StepScale(frames, hz, emulate);
    if (scale > 0) ctx.f1.f64 = double(float(ctx.f1.f64 / scale));
}
