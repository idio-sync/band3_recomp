#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/flags.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include "generated/band3_init.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/Render/sync_gpu/sync_graphics_system.h"
#include "src/settings.h"

extern "C" void __imp__BoxMapLighting__ApplyQueuedLights(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__RndMat__Load(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__OutfitConfig__CompressTextures(PPCContext& ctx, uint8_t* base);

extern "C" REX_FUNC(BoxMapLighting__ApplyQueuedLights)
{
    if (REXCVAR_GET(disable_approximate_lights)) {
        return;
    }
    __imp__BoxMapLighting__ApplyQueuedLights(ctx, base);
}

extern "C" REX_FUNC(RndMat__Load)
{
    uint32_t this_addr = ctx.r3.u32;
    __imp__RndMat__Load(ctx, base);
    if (REXCVAR_GET(disable_hair_shader)) {
        uint32_t shader = REX_LOAD_U32(this_addr + 0x118);
        if (shader == 2) {
			// set shader variation to kShaderVariationNone
            REX_STORE_U32(this_addr + 0x118, 0);
        }
    }
	
    if (REXCVAR_GET(fullbright)) {
		// force useEnviron to be 0
        REX_STORE_U8(this_addr + 0x99, 0);
    }
}

// force_self_shadow. Character::DrawShowing draws a character's self-shadow
// map only with its mSelfShadow set, and BandCamShot::StartAnim sets that from
// each target's self_shadow flag, which some shots turn off (EndAnim turns it
// back on). The flag is set on the shot's targets while StartAnim reads them
// and cleared again after, so the shot's own data stays as loaded; the crowd,
// which turns its characters' off itself, is left alone.
extern "C" void __imp__BandCamShot__StartAnim(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(BandCamShot__StartAnim)
{
    if (!REXCVAR_GET(force_self_shadow)) {
        __imp__BandCamShot__StartAnim(ctx, base);
        return;
    }
    // ObjList<Target> mTargets at 0x19C, a std::list with its head node in the
    // shot: a node's next at 0 and its Target at 8, the Target's flag bits at
    // 0x60 with self_shadow 0x20 (rb3-xenon's BandCamShot.h; StartAnim reads them)
    constexpr uint32_t kTargets = 0x19C;
    constexpr uint32_t kNodeFlags = 8 + 0x60;
    constexpr uint8_t kSelfShadow = 0x20;
    const uint32_t head = ctx.r3.u32 + kTargets;
    uint32_t forced[64];
    size_t count = 0;
    for (uint32_t node = REX_LOAD_U32(head); node != 0 && node != head && count < std::size(forced);
         node = REX_LOAD_U32(node)) {
        const uint8_t flags = REX_LOAD_U8(node + kNodeFlags);
        if (!(flags & kSelfShadow)) {
            REX_STORE_U8(node + kNodeFlags, flags | kSelfShadow);
            forced[count++] = node;
        }
    }
    if (count) REXLOG_DEBUG("force_self_shadow: {} target(s) of shot {:08X}", count, head - kTargets);
    __imp__BandCamShot__StartAnim(ctx, base);
    for (size_t i = 0; i < count; i++) {
        REX_STORE_U8(forced[i] + kNodeFlags, REX_LOAD_U8(forced[i] + kNodeFlags) & ~kSelfShadow);
    }
}

extern "C" void __imp__ProcCounter__ProcCommands(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(ProcCounter__ProcCommands)
{
    if (REXCVAR_GET(disable_even_odd_rendering)) {
        band3::pacing::g_world_period.store(0, std::memory_order_relaxed);
        ctx.r3.u64 = 7;
        return;
    }
    __imp__ProcCounter__ProcCommands(ctx, base);
}

// ProcCounter's fields SetEmulateFPS sets
constexpr uint32_t kProcCounter_Count = 4;     // frames into the period
constexpr uint32_t kProcCounter_Period = 8;    // frames between world frames
constexpr uint32_t kProcCounter_OddHalf = 12;  // its odd half-frame, alternated
constexpr uint32_t kProcCounter_Fps = 16;      // the rate it was set for

// ProcCommands calls this every frame with the venue's emulate_fps; the game
// counts the period as if it ran at 60 (frame_pacing.h). At another rate (the
// frame cap's, or refresh_rate's without it), or with background_fps set, the
// period is counted from the game's real rate instead, stored as
// SetEmulateFPS would.
extern "C" void __imp__ProcCounter__SetEmulateFPS(PPCContext& ctx, uint8_t* base);
extern "C" REX_FUNC(ProcCounter__SetEmulateFPS)
{
    static uint32_t s_counter = 0;
    static int32_t s_half_frames = 0;  // what was stored, 0 when the game set it
    const uint32_t counter = ctx.r3.u32;
    const int32_t fps = ctx.r4.s32;
    const double game_hz = band3::pacing::GameHz();
    const int32_t background_fps = REXCVAR_GET(background_fps);

    const bool at_60 = !(game_hz > 0) || game_hz == 60;
    if (fps <= 0 || (at_60 && background_fps == 0)) {
        // the game's own; one stored here is set again (it keeps a period
        // while the rate it was set for doesn't change, 0 included), so the
        // rate is one no venue asks for
        if (s_half_frames && s_counter == counter) {
            REX_STORE_U32(counter + kProcCounter_Fps, 0x80000000u);
            s_half_frames = 0;
        }
        __imp__ProcCounter__SetEmulateFPS(ctx, base);
    } else {
        const int32_t half_frames = band3::pacing::WorldHalfFrames(game_hz, fps, background_fps);
        if (counter != s_counter || half_frames != s_half_frames ||
            static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_Fps)) != fps) {
            const int32_t period = half_frames >> 1;
            REX_STORE_U32(counter + kProcCounter_Fps, static_cast<uint32_t>(fps));
            REX_STORE_U32(counter + kProcCounter_Period, static_cast<uint32_t>(period));
            REX_STORE_U32(counter + kProcCounter_OddHalf, static_cast<uint32_t>(half_frames & 1));
            if (static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_Count)) >= period)
                REX_STORE_U32(counter + kProcCounter_Count, 0);
            if (half_frames != s_half_frames)
                REXLOG_INFO("Background: the world every {} frames, {:.1f} fps at {:.5g} Hz "
                            "(venue {} fps, background_fps {})",
                            half_frames / 2.0, 2 * game_hz / half_frames, game_hz, fps,
                            background_fps);
            s_counter = counter;
            s_half_frames = half_frames;
        }
        ctx.r3.u64 = static_cast<uint32_t>(fps);
    }
    // what keeps a world frame for the frames after it (scene_capture.cpp)
    band3::pacing::g_world_period.store(
        band3::pacing::MaxPeriod(static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_Period)),
                                 static_cast<int32_t>(REX_LOAD_U32(counter + kProcCounter_OddHalf))),
        std::memory_order_relaxed);
}

// Compressing reads the composed outfits back from guest memory. With
// renderer native nothing composes or resolves them there (the sync-only GPU
// skips every draw), so it would replace them with what memory held, black:
// the setting is ignored then, said once.
extern "C" REX_FUNC(OutfitConfig__CompressTextures)
{
    if (!REXCVAR_GET(compress_character_textures)) {
        return;
    }
    if (band3::render::sync_gpu::NativeOnly()) {
        static std::atomic<bool> said{false};
        if (!said.exchange(true)) {
            REXLOG_INFO("compress_character_textures: ignored with renderer native (no emulated "
                        "GPU composes the outfits to compress; they'd show black)");
        }
        return;
    }
    __imp__OutfitConfig__CompressTextures(ctx, base);
}

// The frame cap's running half (frame_pacing.h).

namespace band3::pacing {

namespace {

using launcher::RefreshRate;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// how often the display is read again, for a window moved to another monitor
// or a monitor given another mode
constexpr auto kDisplayPoll = std::chrono::seconds(2);

// the cap as the game thread reads it each frame; the period is stored last
std::atomic<int64_t> g_period_ns{0};
std::atomic<double> g_cap_hz{0};
std::atomic<FrameCapMode> g_cap_mode{FrameCapMode::kOff};
// the display's rate as last read (DisplayHz), 0 unknown
std::atomic<double> g_display_hz{0};

// the game thread's totals, for GetFrameCapStats
std::atomic<uint64_t> g_frames{0};
std::atomic<uint64_t> g_late{0};
std::atomic<uint64_t> g_resets{0};
std::atomic<int64_t> g_wait_ns{0};
std::atomic<int64_t> g_spin_ns{0};

// what the cap is resolved from, and the thread that reads the display again
struct CapState {
    std::mutex mutex;
    std::condition_variable wake;
    std::thread poller;
    bool running = false;
    bool stop = false;
    bool setting_changed = false;
    void* window = nullptr;
    std::function<void(std::function<void()>)> post_to_ui;
    FrameCapSetting setting;
    RefreshRate display;  // the last rate read
    FrameCap cap;         // what's published
    bool published = false;
};
CapState g_cap;

// vsync as it was before the cap turned it off, while the cap is on
std::mutex g_vsync_mutex;
std::optional<bool> g_user_vsync;

// The SDK's vsync (rexgpu-xenos's) off while the cap is on, and back as it
// was when the cap goes off. Its "GPU VSync" thread reads it again every
// millisecond, so either applies at once. The presenter never reads it: the
// window is presented with a sync interval of 0 either way. With renderer
// native there's no plugin and no vsync setting: band3's sync-only GPU raises
// the vblanks, told directly.
void SetVsyncForCap(bool on) {
    if (auto* sync = render::sync_gpu::Active()) {
        sync->SetVblankFreeRunning(on);
        return;
    }
    std::lock_guard lock(g_vsync_mutex);
    if (on == g_user_vsync.has_value()) return;
    if (on) {
        g_user_vsync = rex::cvar::Query<bool>("vsync");
        if (*g_user_vsync && !rex::cvar::SetFlagByName("vsync", "false"))
            REXLOG_WARN("Frame cap: couldn't turn vsync off, so the vblank holds the game too");
        return;
    }
    // unless it was turned on again meanwhile (F4)
    if (*g_user_vsync && !rex::cvar::Query<bool>("vsync")) rex::cvar::SetFlagByName("vsync", "true");
    g_user_vsync.reset();
}

void LogCap(const FrameCap& cap, const FrameCapSetting& setting, RefreshRate display) {
    const double ms = cap.period_ns / 1e6;
    switch (cap.mode) {
    case FrameCapMode::kDisplay:
        REXLOG_INFO("Frame cap: display {:.2f} Hz -> {:.3f} ms", cap.hz, ms);
        return;
    case FrameCapMode::kAuto:
        REXLOG_INFO("Frame cap: auto {:.2f} Hz (display {:.2f} Hz) -> {:.3f} ms", cap.hz,
                    double(display.num) / display.den, ms);
        return;
    case FrameCapMode::kFixed:
        REXLOG_INFO("Frame cap: {:.2f} Hz -> {:.3f} ms", cap.hz, ms);
        return;
    case FrameCapMode::kOff: break;
    }
    if (setting.mode == FrameCapMode::kOff)
        REXLOG_INFO("Frame cap: off, the emulated vblank paces the game");
    else
        REXLOG_INFO("Frame cap: {}, but the display's refresh rate can't be told, so it's off",
                    FrameCapModeName(setting.mode));
}

// publishes the display's rate and what the setting comes to on it now,
// logging the cap when that changes; whether the cap is on. g_cap.mutex held.
bool Publish() {
    const FrameCap cap = ResolveFrameCap(g_cap.setting, g_cap.display.num, g_cap.display.den);
    g_display_hz.store(g_cap.display.den ? double(g_cap.display.num) / g_cap.display.den : 0,
                       std::memory_order_relaxed);
    if (!g_cap.published || cap.mode != g_cap.cap.mode || cap.period_ns != g_cap.cap.period_ns) {
        LogCap(cap, g_cap.setting, g_cap.display);
        g_cap.cap = cap;
        g_cap.published = true;
        g_cap_hz.store(cap.hz, std::memory_order_relaxed);
        g_cap_mode.store(cap.mode, std::memory_order_relaxed);
        g_period_ns.store(cap.period_ns, std::memory_order_release);
    }
    return cap.period_ns > 0;
}

void Poll() {
    std::unique_lock lock(g_cap.mutex);
    while (!g_cap.stop) {
        g_cap.wake.wait_for(lock, kDisplayPoll, [] { return g_cap.stop || g_cap.setting_changed; });
        if (g_cap.stop) break;
        g_cap.setting_changed = false;
        void* window = g_cap.window;
        lock.unlock();
        const RefreshRate rate = launcher::DisplayRefresh(window);
        lock.lock();
        // a display that can't be read for a moment (changing modes, a remote
        // session reconnecting) keeps the rate it had
        if (rate.num) g_cap.display = rate;
        const bool was_on = g_cap.cap.period_ns > 0;
        const bool on = Publish();
        if (on != was_on && g_cap.post_to_ui) g_cap.post_to_ui([on] { SetVsyncForCap(on); });
    }
}

// The guest's refresh rate (refresh_rate) to the cap's when it isn't set, so
// the game and ProcCounter count from the rate it runs at, and the vblank
// keeps it if the cap goes off later. The "GPU VSync" thread reads it once,
// as the GPU starts, so only at startup.
void FollowCapWithGuestRate(double cap_hz) {
    const double guest = REXCVAR_GET(video_mode_refresh_rate);
    const bool unset =
        rex::cvar::GetFlagSource("video_mode_refresh_rate") == rex::cvar::Source::kDefault ||
        !(guest > 0);
    if (!unset) {
        REXLOG_INFO("Frame cap: refresh_rate is set to {}, so the game keeps it", guest);
        return;
    }
    // VdQueryVideoMode keeps it to 24..240, as frame_cap's numbers are
    const long hz = std::lround(cap_hz);
    if (guest == double(hz)) return;
    if (rex::cvar::SetFlagByName("video_mode_refresh_rate", std::to_string(hz)))
        REXLOG_INFO("Frame cap: refresh_rate {} to go with it", hz);
    else
        REXLOG_WARN("Frame cap: couldn't set refresh_rate to {}", hz);
}

}

std::optional<bool> VsyncBeforeCap() {
    std::lock_guard lock(g_vsync_mutex);
    return g_user_vsync;
}

void StartFrameCap(void* native_window, std::function<void(std::function<void()>)> post_to_ui) {
    std::unique_lock lock(g_cap.mutex);
    if (g_cap.running) return;
    g_cap.window = native_window;
    g_cap.post_to_ui = std::move(post_to_ui);
    g_cap.setting = ParseFrameCap(REXCVAR_GET(frame_cap)).value_or(FrameCapSetting{});
    g_cap.display = launcher::DisplayRefresh(native_window);
    const bool on = Publish();
    const double hz = g_cap.cap.hz;
    lock.unlock();
    if (on) {
        SetVsyncForCap(true);
        FollowCapWithGuestRate(hz);
        // band3's own changes, which F4 needn't flag as waiting on a restart
        rex::cvar::ClearPendingRestartFlags();
    }
    rex::cvar::RegisterChangeCallback("frame_cap", [](std::string_view, std::string_view value) {
        const auto setting = ParseFrameCap(value);
        if (!setting) return;
        std::lock_guard lock(g_cap.mutex);
        g_cap.setting = *setting;
        g_cap.setting_changed = true;
        g_cap.wake.notify_one();
    });
    lock.lock();
    g_cap.running = true;
    g_cap.stop = false;
    g_cap.poller = std::thread(Poll);
}

void StopFrameCap() {
    std::thread poller;
    {
        std::lock_guard lock(g_cap.mutex);
        if (!g_cap.running) return;
        g_cap.running = false;
        g_cap.stop = true;
        poller = std::move(g_cap.poller);
    }
    g_cap.wake.notify_all();
    if (poller.joinable()) poller.join();
}

void PaceFrame() {
    // Present runs on RB3's splash thread at boot and its main thread after,
    // never both at once, so they share the beat; the lock is never contended
    static std::mutex mutex;
    static FrameCapSchedule schedule;
    const int64_t period = g_period_ns.load(std::memory_order_acquire);
    const int64_t now = NowNs();
    int64_t go = now;
    {
        std::lock_guard lock(mutex);
        schedule.SetPeriod(period);
        if (period <= 0) return;
        go = schedule.Next(now);
        g_late.store(schedule.Late(), std::memory_order_relaxed);
        g_resets.store(schedule.Resets(), std::memory_order_relaxed);
    }
    g_frames.fetch_add(1, std::memory_order_relaxed);
    if (go <= now) return;
    const int64_t spin = WaitUntil(go);
    g_wait_ns.fetch_add(NowNs() - now, std::memory_order_relaxed);
    g_spin_ns.fetch_add(spin, std::memory_order_relaxed);
}

double GameHz() {
    const double cap = g_cap_hz.load(std::memory_order_relaxed);
    return cap > 0 ? cap : REXCVAR_GET(video_mode_refresh_rate);
}

double DisplayHz() { return g_display_hz.load(std::memory_order_relaxed); }

FrameCapStats GetFrameCapStats() {
    FrameCapStats out;
    out.mode = g_cap_mode.load(std::memory_order_relaxed);
    out.hz = g_cap_hz.load(std::memory_order_relaxed);
    out.frames = g_frames.load(std::memory_order_relaxed);
    out.late = g_late.load(std::memory_order_relaxed);
    out.resets = g_resets.load(std::memory_order_relaxed);
    out.wait_ms = g_wait_ns.load(std::memory_order_relaxed) / 1e6;
    out.spin_ms = g_spin_ns.load(std::memory_order_relaxed) / 1e6;
    return out;
}

}
