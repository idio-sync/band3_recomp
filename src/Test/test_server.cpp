#include "test_server.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <tlhelp32.h>
#include <cwchar>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>
#include <rex/ui/window_sdl.h>
#include <rex/ui/windowed_app_context.h>
#include <rex/input/input_system.h>
#include "src/Audio/usb_mic_capture.h"
#include "src/Content/live_content.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/keyboard_search_driver.h"
#include "src/Input/virtual_instrument.h"
#include "src/Input/xinput_state.h"
#include "src/Net/liveless_rooms.h"
#include "src/Net/online_hooks.h"
#include "src/Net/port_mapping.h"
#include "src/Render/capture_file.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/gpu_view.h"
#include "src/Render/native_view.h"
#include "src/Render/png_writer.h"
#include "src/Render/scene_capture.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/Render/sync_gpu/sync_graphics_system.h"
#include "src/Render/sync_gpu/sync_monitor.h"
#include "src/settings.h"
#include "game_state.h"
#include "test_commands.h"

namespace band3::test {

namespace {

using rex::X_RESULT;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif

// The CPU time (kernel and user) the emulated GPU's command processor thread
// has used, in milliseconds, or -1 where it can't be told: what the emulated
// GPU still costs while the native renderer draws (native_view stats). The
// SDK names the thread "GPU Commands"; it's looked for in a snapshot of the
// process's threads until found, then only read, at each stats.
double CpThreadMs() {
#ifdef _WIN32
    static std::mutex mutex;
    static HANDLE thread = nullptr;
    std::lock_guard lock(mutex);
    if (!thread) {
        using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
        const auto get_description = reinterpret_cast<GetDescription>(reinterpret_cast<void*>(
            GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
        HANDLE snap = get_description ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
                                      : INVALID_HANDLE_VALUE;
        if (snap == INVALID_HANDLE_VALUE) return -1;
        const DWORD pid = GetCurrentProcessId();
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        for (BOOL ok = Thread32First(snap, &te); ok && !thread; ok = Thread32Next(snap, &te)) {
            if (te.th32OwnerProcessID != pid) continue;
            HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
            if (!h) continue;
            PWSTR name = nullptr;
            if (SUCCEEDED(get_description(h, &name)) && name) {
                if (std::wcsstr(name, L"GPU Commands")) thread = h;
                LocalFree(name);
            }
            if (thread != h) CloseHandle(h);
        }
        CloseHandle(snap);
        if (!thread) return -1;
    }
    FILETIME created, exited, kernel, user;
    if (!GetThreadTimes(thread, &created, &exited, &kernel, &user)) return -1;
    auto ticks = [](const FILETIME& f) {
        return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    // 100 ns ticks
    return double(ticks(kernel) + ticks(user)) / 1e4;
#else
    return -1;
#endif
}

// how often the server thread looks up from its sockets to see if it should stop
constexpr std::chrono::milliseconds kStopCheck{200};
// longer than any command could want, so one can't grow without bound
constexpr size_t kMaxLine = 4096;

int32_t g_port = 0;

// the game, as the commands see it
class GameTarget final : public TestTarget {
public:
    GameTarget(rex::Runtime* runtime, rex::ui::WindowedAppContext* app_context,
               rex::ui::Window* window, const std::atomic<bool>& stopping)
        : runtime_(runtime), app_context_(app_context), window_(window), stopping_(stopping) {}

    std::optional<input::InstrumentKind> Kind(int player) override {
        const auto& instrument = input::VirtualInstrument::ForPlayer(player);
        if (!instrument.plugged()) return std::nullopt;
        return instrument.kind();
    }

    // the settings' instrument changes through the settings, on the UI thread
    void Plug(int player, input::InstrumentKind kind) override {
        OnUIThread([player, kind] { input::VirtualInstrument::ForPlayer(player).Plug(kind); });
    }

    void Unplug(int player) override {
        OnUIThread([player] { input::VirtualInstrument::ForPlayer(player).Unplug(); });
    }

    input::InstrumentInputs Held(int player) override {
        return input::VirtualInstrument::ForPlayer(player).Held();
    }

    void SetHeld(int player, const input::InstrumentInputs& in) override {
        input::VirtualInstrument::ForPlayer(player).SetHeld(in);
    }

    void Pulse(int player, std::function<void(input::InstrumentInputs&)> change,
               std::chrono::milliseconds length) override {
        input::VirtualInstrument::ForPlayer(player).Pulse(std::move(change), length);
    }

    GameStateSnapshot State() override {
        GameStateSnapshot state = GameState::Get().Snapshot();
        if (audio::UsbMicsRunning()) {
            for (const audio::UsbMicSlotStatus& slot : audio::GetUsbMicStatus().slots) {
                state.mics.push_back({slot.device, slot.connected, slot.bytes_fed});
            }
        }
        state.rooms_state = rooms::StateName(rooms::GetStatus().state);
        state.port_mapping_state = port_mapping::StateName(port_mapping::GetStatus().state);
        return state;
    }

    // the window's picture, as the renderer setting has it, or the one asked
    // for; not the emulated GPU's while it skips the game's draws, which
    // isn't the game's picture (capture asks for whole frames first)
    std::string Screenshot(const std::string& name, ScreenshotSource source,
                           ScreenshotInfo& out) override {
        // renderer native: the window's picture is the native renderer's,
        // and there's no other
        if (render::sync_gpu::NativeOnly()) {
            if (source == ScreenshotSource::kEmulated)
                return "no emulated GPU this run (renderer native)";
            return Shot(name, true, out);
        }
        const bool native = source == ScreenshotSource::kNative ||
                            (source == ScreenshotSource::kWindow && render::NativePresenting());
        if (!native && !render::EmulatedPictureFresh()) {
            return "the emulated GPU's picture is stale: it skips the game's draws while the "
                   "native renderer draws the window (emulated_gpu_while_native skip_draws or "
                   "swap_only). `capture` has it draw whole frames first (under swap_only, "
                   "what RB3 drew once may still be black); or set emulated_gpu_while_native "
                   "full";
        }
        return Shot(name, native, out);
    }

    // the native renderer's picture or the emulated GPU's, fresh or not
    std::string Shot(const std::string& name, bool native, ScreenshotInfo& out) {
        std::vector<uint32_t> rgba;
        uint32_t width = 0, height = 0;
        const std::string error =
            native ? NativePicture(rgba, width, height) : EmulatedPicture(rgba, width, height);
        if (!error.empty()) return error;

        const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "screenshots";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path path = dir / ((name.empty() ? TimestampName() : name) + ".png");
        if (!render::WritePng(path.string(), rgba, width, height)) {
            return "couldn't write " + path.string();
        }
        out.path = path.string();
        out.width = width;
        out.height = height;
        out.renderer = native ? "native" : "emulated";
        return {};
    }

    // the emulated GPU's picture, at the guest's size
    std::string EmulatedPicture(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height) {
        auto* graphics = runtime_ ? runtime_->graphics_system() : nullptr;
        rex::ui::Presenter* presenter = graphics ? graphics->presenter() : nullptr;
        if (!presenter) return "there is no picture to capture (no presenter)";
        rex::ui::RawImage image;
        if (!presenter->CaptureGuestOutput(image) || !image.width || !image.height) {
            return "the game hasn't drawn a frame yet";
        }
        // R8 G8 B8 X8 rows to RGBA, R in the low byte
        rgba.resize(size_t(image.width) * image.height);
        for (uint32_t y = 0; y < image.height; y++) {
            const uint8_t* row = image.data.data() + y * image.stride;
            for (uint32_t x = 0; x < image.width; x++) {
                const uint8_t* p = row + x * 4;
                rgba[size_t(y) * image.width + x] =
                    p[0] | (p[1] << 8) | (p[2] << 16) | 0xFF000000u;
            }
        }
        width = image.width;
        height = image.height;
        return {};
    }

    // The native renderer's picture: while it draws the window, the next
    // frame it draws, at the window's picture's size; otherwise the game's
    // next whole frame drawn once at 1280x720, on native_view_backend
    std::string NativePicture(std::vector<uint32_t>& rgba, uint32_t& width, uint32_t& height) {
        if (render::NativePresenting())
            return render::NativePresentedPicture(rgba, width, height, std::chrono::seconds(3));
        auto frame = render::CaptureHeldFrame([] {}, std::chrono::seconds(5),
                                              std::chrono::milliseconds(0));
        if (!frame) return "the game didn't finish a frame to draw in 5 s";
        render::RasterOptions options;
        options.width = width = 1280;
        options.height = height = 720;
        options.normal_maps = REXCVAR_GET(native_view_normal_maps);
        options.filtering = REXCVAR_GET(native_view_texture_filtering);
        options.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
        // as the native renderer's frames keep what they draw, so this one
        // doesn't let it go (gpu_view.h's residency)
        options.clock_keep = render::InSong();
        bool ready = false;
        // SDL starts video on the main thread only
        if (REXCVAR_GET(native_view_backend) == "gpu")
            OnUIThread([&] { ready = render::GpuRenderer::Get().Init(); });
        render::GpuStats stats;
        if (!ready || !render::GpuRenderer::Get().RenderFrame(*frame, options, rgba, stats))
            render::Rasterize(*frame, options, rgba);
        return {};
    }

    // the game is held at the end of the captured frame while the screenshot
    // is taken, so both are the same frame. While the emulated GPU skips the
    // game's draws (renderer native), it draws whole frames for a while
    // first, enough for the held frame's wait (CaptureHeldFrame:
    // kWholeFramesToHold whole in a row, two to learn, thirty at most), so
    // the screenshot is the game's picture of the captured frame.
    // With renderer native there's no emulated picture: no whole frames
    // first, and the screenshot is the native renderer's (its drawing of the
    // held frame, the newest captured), `emulated` "none".
    std::string Capture(const std::string& name, CaptureInfo& out) override {
        const std::string file = name.empty() ? TimestampName() : name;
        std::string shot_error;
        const bool native_only = render::sync_gpu::NativeOnly();
        if (!native_only) render::RequestFullFrames(render::kWholeFramesToHold + 40);
        auto frame = render::CaptureHeldFrame(
            [&] {
                if (native_only) {
                    out.emulated = "none";
                    shot_error = Shot(file, true, out.screenshot);
                    return;
                }
                out.emulated = render::EmulatedPictureFresh() ? "full" : "stale";
                shot_error = Shot(file, false, out.screenshot);
            },
            std::chrono::seconds(5),
            std::chrono::milliseconds(150), &out.held_fallback);
        if (!frame) return "the game didn't finish a frame to capture in 5 s";
        if (!shot_error.empty()) return shot_error;
        out.emulated_passes_dropped = render::GetGpuSkipStats().passes_dropped;
        const std::filesystem::path path =
            rex::filesystem::GetExecutableFolder() / "screenshots" / (file + ".cap");
        if (!render::SaveCapture(path.string(), *frame)) return "couldn't write " + path.string();
        out.capture_path = path.string();
        out.frame = frame->frame;
        out.draws = uint32_t(frame->draws.size());
        out.skipped_shadow = frame->skipped_shadow;
        out.skipped_pass = frame->skipped_velocity + frame->skipped_draw_mode;
        uint32_t texture_passes = 0;
        for (const render::Pass& p : frame->passes)
            if (p.tex_obj) texture_passes++;
        out.passes = texture_passes;
        out.passes_carried = frame->passes_carried;
        out.rt_sampled = frame->rt_sampled;
        out.rt_missing = frame->rt_missing;
        out.rt_filtered = frame->rt_filtered;
        out.rt_fallback = render::RtFallbackGuest() ? "guest" : "none";
        out.proc_cmds = render::ProcKnown(*frame) ? int64_t(frame->proc_cmds) : -1;
        out.composed = frame->composed != 0;
        out.game_frame = frame->game_frame;
        out.world_frame = frame->world_frame;
        GpuCapture(*frame, file, out);
        return {};
    }

    // the native view's GPU backend draws the same frame, as <name>.gpu.png,
    // after the game has gone on so it isn't held any longer; the capture
    // stands without it, and with native_view_backend = cpu there's no GPU
    // device to make
    void GpuCapture(const render::FrameCapture& frame, const std::string& file,
                    CaptureInfo& out) {
        if (REXCVAR_GET(native_view_backend) != "gpu") {
            out.gpu_error = "native_view_backend is " + REXCVAR_GET(native_view_backend) +
                            ", so no GPU drawing";
            return;
        }
        bool ready = false;
        // SDL starts video on the main thread only
        OnUIThread([&] { ready = render::GpuRenderer::Get().Init(); });
        if (!ready) {
            out.gpu_error = "no GPU device for the native view (see the log)";
            return;
        }
        render::RasterOptions options;
        options.width = out.screenshot.width;
        options.height = out.screenshot.height;
        // renderer native: the screenshot is the native renderer's, at the
        // window's size, so this one is at the game's, as with the emulated
        // GPU (the window's size is .gpu.presented.png's)
        if (render::sync_gpu::NativeOnly()) {
            options.width = 1280;
            options.height = 720;
        }
        options.normal_maps = REXCVAR_GET(native_view_normal_maps);
        options.filtering = REXCVAR_GET(native_view_texture_filtering);
        options.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
        options.clock_keep = render::InSong();  // as NativePicture's
        std::vector<uint32_t> rgba;
        render::GpuStats stats;
        if (!render::GpuRenderer::Get().RenderFrame(frame, options, rgba, stats)) {
            out.gpu_error = "the GPU didn't draw the frame (see the log)";
            return;
        }
        const std::filesystem::path path =
            rex::filesystem::GetExecutableFolder() / "screenshots" / (file + ".gpu.png");
        if (!render::WritePng(path.string(), rgba, options.width, options.height)) {
            out.gpu_error = "couldn't write " + path.string();
            return;
        }
        out.gpu_path = path.string();
        out.gpu_ms = stats.ms;
        out.gpu_wait_ms = stats.wait_ms;
        out.gpu_passes = stats.passes;
        out.gpu_rt_missing = stats.rt_missing;
        // and at the size the native renderer draws the window at, if it
        // does and that's another, with the passes it scales scaled
        // (<name>.gpu.presented.png)
        uint32_t pw = 0, ph = 0;
        if (render::NativePresentDrawSize(pw, ph) &&
            (pw != options.width || ph != options.height)) {
            render::RasterOptions presented = options;
            presented.width = pw;
            presented.height = ph;
            render::ScaleForPicture(presented);
            std::vector<uint32_t> big;
            render::GpuStats presented_stats;
            const std::filesystem::path big_path = rex::filesystem::GetExecutableFolder() /
                                                   "screenshots" / (file + ".gpu.presented.png");
            if (render::GpuRenderer::Get().RenderFrame(frame, presented, big, presented_stats) &&
                render::WritePng(big_path.string(), big, pw, ph))
                out.gpu_presented_path = big_path.string();
        }
        // and its scene target's alpha and depth, as grey, beside it
        // (<name>.gpu.alpha.png, .gpu.depth.png): what replay's --dump-alpha
        // and --dump-depth show of the CPU's
        for (auto [view, suffix] : {std::pair{render::RasterView::kSceneAlpha, ".gpu.alpha.png"},
                                    std::pair{render::RasterView::kSceneDepth, ".gpu.depth.png"}}) {
            options.view = view;
            render::GpuStats view_stats;
            if (!render::GpuRenderer::Get().RenderFrame(frame, options, rgba, view_stats)) break;
            render::WritePng(
                (rex::filesystem::GetExecutableFolder() / "screenshots" / (file + suffix)).string(),
                rgba, options.width, options.height);
        }
        // and, sampling the textures as it did before the game's samplers,
        // nearest at level 0, <name>.gpu.nearest.png: the same frame without
        // them, to see what they change
        if (options.filtering) {
            options.view = render::RasterView::kFinal;
            options.filtering = false;
            render::GpuStats nearest_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, options, rgba, nearest_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.nearest.png"))
                                     .string(),
                                 rgba, options.width, options.height);
        }
    }

    std::string NativeViewOn(uint32_t width, uint32_t height, bool sized, bool post) override {
        if (sized && render::NativePresenting()) {
            return "renderer is native: the native renderer draws the window and its size "
                   "follows the window's; native_view on without a size measures it";
        }
        // the GPU device starts on the UI thread
        OnUIThread([&] { render::StartLiveView(width, height, post); });
        StartMeasuring();
        return {};
    }

    void NativeViewOff() override {
        render::StopLiveView();
        StartMeasuring();
    }

    NativeViewStats NativeView() override {
        NativeViewStats out;
        out.on = render::LiveViewOn();
        render::LiveViewStats live = render::GetLiveViewStats();
        // while it's off, only the game is measured
        if (out.on) {
            out.backend = live.gpu ? "gpu" : "cpu";
            out.width = live.width;
            out.height = live.height;
            out.post = live.post;
            out.captured = live.captured;
            out.rendered = live.rendered;
            out.skipped_busy = live.skipped_busy;
            out.worldless = live.worldless;
            out.frame_ms = std::move(live.ms);
            out.wait_ms = std::move(live.wait_ms);
            out.in_flight_max = live.in_flight_max;
            out.paused = live.paused;
            out.paused_ms = live.paused_ms;
            out.paused_captures = live.paused_captures;
            out.camera_cuts = live.camera_cuts;
            out.vel_resets = live.vel_resets;
            out.cuts_confirmed = live.cuts_confirmed;
            for (int k = 0; k < render::kFrameKinds; k++)
                out.by_kind.push_back(ByKind(render::FrameKind(k), live.by_kind[k]));
        }
        const render::PassRecordingStats rec = render::GetPassRecordingStats();
        const render::CaptureProfile profile = render::GetCaptureProfile();
        std::lock_guard lock(measure_mutex_);
        out.seconds = std::chrono::duration<double>(Clock::now() - measure_start_).count();
        out.game_frames = GameState::Get().Snapshot().frame - measure_frame_;
        out.rt_on = rec.on;
        out.rt_passes = rec.passes - measure_rec_.passes;
        out.rt_recorded = rec.passes_recorded - measure_rec_.passes_recorded;
        out.rt_draws = rec.draws_recorded - measure_rec_.draws_recorded;
        out.rt_ms = rec.ms - measure_rec_.ms;
        out.capture = CaptureCost(render::CaptureProfileSince(profile, measure_profile_));
        out.emulated_gpu =
            EmulatedGpu(render::GpuSkipStatsSince(render::GetGpuSkipStats(), measure_gpu_));
        const double cp_ms = CpThreadMs();
        out.emulated_gpu.cp_ms = cp_ms >= 0 && measure_cp_ms_ >= 0 ? cp_ms - measure_cp_ms_ : -1;
        if (render::sync_gpu::NativeOnly()) {
            out.emulated_gpu.present = false;
            if (auto* sync = render::sync_gpu::Active())
                out.emulated_gpu.sync = SyncGpu(sync->Stats(), measure_sync_);
        }
        return out;
    }

    // the window's paints as the native renderer's drawer timed them, and
    // the game's Presents in the same time
    PresentStats Present(bool reset) override {
        PresentStats out;
        const auto now = Clock::now();
        render::PresentPaintStats paints = render::GetPresentPaintStats(reset);
        // just after a reset starts the paints' stretch over, so the next
        // stretch's frames are counted from when it began
        const uint64_t presented = render::GamePresentCount();
        out.renderer = render::NativePresenting() ? "native" : "emulated";
        out.path = paints.path;
        out.seconds = std::chrono::duration<double>(now - paints.since).count();
        out.paints = paints.log.paints;
        out.paint_ms = std::move(paints.log.interval_ms);
        out.native_paints = paints.log.native_paints;
        out.shown = paints.log.shown;
        out.repeats = paints.log.repeats;
        out.skipped = paints.log.skipped;
        out.latency_ms = std::move(paints.log.latency_ms);
        out.publish_latency_ms = std::move(paints.log.publish_latency_ms);
        // the intervals are the stretch's last few thousand only (the ones
        // GamePresentTimes keeps); its frames are all of them, counted
        const auto presents = render::GamePresentTimes(paints.since);
        for (size_t i = 1; i < presents.size(); i++)
            out.game_ms.push_back(
                std::chrono::duration<double, std::milli>(presents[i] - presents[i - 1]).count());
        // the cap's since present_stats last started over (or since startup)
        const pacing::FrameCapStats cap = pacing::GetFrameCapStats();
        std::lock_guard lock(measure_mutex_);
        out.game_frames = presented - present_frames_;
        if (reset) present_frames_ = presented;
        const pacing::FrameCapStats& from = present_cap_;
        const uint64_t frames = cap.frames - from.frames;
        out.cap.mode = pacing::FrameCapModeName(cap.mode);
        out.cap.hz = cap.hz;
        out.cap.late = cap.late - from.late;
        out.cap.resets = cap.resets - from.resets;
        out.cap.wait_ms = frames ? (cap.wait_ms - from.wait_ms) / double(frames) : 0;
        out.cap.spin_ms = frames ? (cap.spin_ms - from.spin_ms) / double(frames) : 0;
        if (reset) present_cap_ = cap;
        return out;
    }

    std::string SetSetting(std::string_view name, std::string_view value) override {
        const rex::cvar::FlagEntry* info = rex::cvar::GetFlagInfo(name);
        if (!info || !info->category.starts_with("Band3/")) {
            return std::string(name) + " isn't a Band3 setting";
        }
        // the SDK takes any word for a boolean, and anything but "true" is false
        if (info->type == rex::cvar::FlagType::Boolean && value != "true" && value != "false" &&
            value != "1" && value != "0") {
            return std::string(name) + " is true or false";
        }
        bool set = false;
        OnUIThread([&] { set = rex::cvar::SetFlagByName(name, value); });
        if (!set) return std::string(name) + " doesn't take " + std::string(value);
        return {};
    }

    std::optional<SettingValue> GetSetting(std::string_view name) override {
        std::optional<SettingValue> out;
        OnUIThread([&] {
            if (!rex::cvar::GetFlagInfo(name)) return;
            out = SettingValue{rex::cvar::GetFlagByName(name),
                               SourceName(rex::cvar::GetFlagSource(name))};
        });
        return out;
    }

    GameFolders Folders() override {
        GameFolders out{rex::path_to_utf8(runtime_->game_data_root()),
                        rex::path_to_utf8(runtime_->user_data_root()),
                        rex::path_to_utf8(runtime_->cache_root()),
                        {}};
        std::string setting;
        OnUIThread([&] { setting = REXCVAR_GET(content_folders); });
        for (const auto& folder : content::ContentFolders(setting)) {
            out.content.push_back(rex::path_to_utf8(folder));
        }
        return out;
    }

    // as band3_app's PressBind does for the menu shortcut: the bind's key goes
    // straight to the binds, so it works on a window that never has focus
    std::string PressBind(std::string_view bind) override {
        std::string error;
        rex::ui::Window* window = window_;
        OnUIThread([&] {
            // empty for a name no bind has
            const std::string key = rex::cvar::GetFlagByName(bind);
            const rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(key);
            if (vk == rex::ui::VirtualKey::kNone) {
                error = "no key bind " + std::string(bind) + " (or it has no key)";
                return;
            }
            rex::ui::KeyEvent e(window, vk, 0, false, false, false, false, false);
            rex::ui::ProcessKeyEvent(e);
        });
        return error;
    }

    // As the window gets keys with focus: SDL's events, handed to the window
    // as its app context does (SDL only sends keys to the focused window).
    // Each key goes down, then its character if the window was taking text as
    // it went down (SDL makes the text before the window sees the key), then
    // up. Tokens are as test_commands.h's TypeKeys says.
    std::string TypeKeys(const std::vector<std::string>& tokens) override {
        auto* window = static_cast<rex::ui::WindowSDL*>(window_);
        if (!window) return "no window";
        OnUIThread([&] {
            input::TypeAsFocused(true);
            const auto press = [window](SDL_Scancode scancode, SDL_Keycode key, bool shift,
                                        char text) {
                const bool taking_text = window->IsTextInputActive();
                SDL_Event event{};
                event.type = SDL_EVENT_KEY_DOWN;
                event.key.scancode = scancode;
                event.key.key = key;
                event.key.mod = shift ? SDL_KMOD_LSHIFT : SDL_KMOD_NONE;
                event.key.down = true;
                window->HandleKeyEvent(event);
                if (text && taking_text) {
                    const char typed[2] = {text, 0};
                    SDL_Event text_event{};
                    text_event.type = SDL_EVENT_TEXT_INPUT;
                    text_event.text.text = typed;
                    window->HandleTextInputEvent(text_event);
                }
                event.type = SDL_EVENT_KEY_UP;
                event.key.down = false;
                window->HandleKeyEvent(event);
            };
            for (const std::string& token : tokens) {
                if (token.starts_with('{')) {
                    const std::string_view name = std::string_view(token).substr(1, token.size() - 2);
                    if (name == "enter") press(SDL_SCANCODE_RETURN, SDLK_RETURN, false, 0);
                    if (name == "back") press(SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE, false, 0);
                    if (name == "tab") press(SDL_SCANCODE_TAB, SDLK_TAB, false, 0);
                    if (name == "esc") press(SDL_SCANCODE_ESCAPE, SDLK_ESCAPE, false, 0);
                    if (name == "space") press(SDL_SCANCODE_SPACE, SDLK_SPACE, false, ' ');
                    if (name == "left") press(SDL_SCANCODE_LEFT, SDLK_LEFT, false, 0);
                    if (name == "right") press(SDL_SCANCODE_RIGHT, SDLK_RIGHT, false, 0);
                    if (name == "up") press(SDL_SCANCODE_UP, SDLK_UP, false, 0);
                    if (name == "down") press(SDL_SCANCODE_DOWN, SDLK_DOWN, false, 0);
                    if (name == "delete") press(SDL_SCANCODE_DELETE, SDLK_DELETE, false, 0);
                    continue;
                }
                for (const char c : token) {
                    // a US keyboard's key for it; SDL's key codes are the
                    // unshifted characters
                    if (c >= 'a' && c <= 'z') {
                        press(static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'a')), c, false, c);
                    } else if (c >= 'A' && c <= 'Z') {
                        const char lower = static_cast<char>(c - 'A' + 'a');
                        press(static_cast<SDL_Scancode>(SDL_SCANCODE_A + (c - 'A')), lower, true, c);
                    } else if (c == '0') {
                        press(SDL_SCANCODE_0, c, false, c);
                    } else if (c >= '1' && c <= '9') {
                        press(static_cast<SDL_Scancode>(SDL_SCANCODE_1 + (c - '1')), c, false, c);
                    } else {
                        const SDL_Scancode scancode = c == '-'    ? SDL_SCANCODE_MINUS
                                                      : c == '.'  ? SDL_SCANCODE_PERIOD
                                                      : c == ','  ? SDL_SCANCODE_COMMA
                                                      : c == '\'' ? SDL_SCANCODE_APOSTROPHE
                                                      : c == '/'  ? SDL_SCANCODE_SLASH
                                                                  : SDL_SCANCODE_GRAVE;
                        press(scancode, c, false, c);
                    }
                }
            }
            input::TypeAsFocused(false);
        });
        return {};
    }

    std::string LivelessInvite(const std::string& host, uint16_t port, bool force_flag) override {
        return online::FakeInvite(host, port, force_flag);
    }

    rooms::Status RoomsStatus() override { return rooms::GetStatus(); }

    std::string RoomsJoin(const std::string& code) override { return rooms::Join(code); }

    std::string RoomsConnect() override { return rooms::Connect(); }

    port_mapping::Status PortMappingStatus() override { return port_mapping::GetStatus(); }

    // as the window's close button: RequestClose skips the close request the
    // button makes (band3_app.h's OnWindowCloseRequested), so online play
    // lets go of the router's port mapping and the Rooms server here first
    void Quit() override {
        rex::ui::Window* window = window_;
        app_context_->CallInUIThreadDeferred([window] {
            online::Stop();
            window->RequestClose();
        });
    }

    bool Cancelled() override { return stopping_.load(); }

    bool ReadPad(int player, input::Gamepad360& out, uint32_t& packet) override {
        rex::input::InputSystem* system = input::GameInputSystem();
        if (!system) return false;
        rex::input::X_INPUT_STATE state{};
        {
            std::lock_guard<std::recursive_mutex> lock(input::InputLock());
            // ForUI: the same read, without consuming the buttons a dialog took
            if (system->GetStateForUI(static_cast<uint32_t>(player - 1), &state) !=
                X_ERROR_SUCCESS) {
                return false;
            }
        }
        out = input::LoadGamepad(state.gamepad);
        packet = state.packet_number;
        return true;
    }

    Clock::time_point Now() override { return Clock::now(); }

    void Sleep(std::chrono::milliseconds length) override {
        // in steps, so shutting down doesn't wait out a long press
        const auto end = Clock::now() + length;
        while (!stopping_.load()) {
            const auto now = Clock::now();
            if (now >= end) break;
            std::this_thread::sleep_for(std::min<Clock::duration>(end - now, kStopCheck));
        }
    }

private:
    // cvars and their change callbacks belong to the UI thread
    void OnUIThread(const std::function<void()>& function) {
        app_context_->CallInUIThreadSynchronous(function);
    }

    static std::string SourceName(rex::cvar::Source source) {
        switch (source) {
        case rex::cvar::Source::kDefault: return "default";
        case rex::cvar::Source::kConfig: return "config";
        case rex::cvar::Source::kEnvironment: return "environment";
        case rex::cvar::Source::kCommandLine: return "command_line";
        case rex::cvar::Source::kRuntime: return "runtime";
        }
        return "unknown";
    }

    static std::string TimestampName() {
        const auto now = std::chrono::system_clock::now();
        const std::time_t t = std::chrono::system_clock::to_time_t(now);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            now.time_since_epoch()).count() % 1000;
        std::tm tm{};
#ifdef _WIN32
        localtime_s(&tm, &t);
#else
        localtime_r(&t, &tm);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
        return std::string(buf) + "-" + std::to_string(ms);
    }

    // the game's frame rate is measured from the last native_view on or off
    void StartMeasuring() {
        const uint64_t frame = GameState::Get().Snapshot().frame;
        const render::PassRecordingStats rec = render::GetPassRecordingStats();
        const render::CaptureProfile profile = render::GetCaptureProfile();
        const render::GpuSkipStats gpu = render::GetGpuSkipStats();
        const double cp_ms = CpThreadMs();
        // renderer native: the sync-only GPU's numbers too
        auto* sync = render::sync_gpu::Active();
        std::lock_guard lock(measure_mutex_);
        measure_start_ = Clock::now();
        measure_frame_ = frame;
        measure_rec_ = rec;
        measure_profile_ = profile;
        measure_gpu_ = gpu;
        measure_cp_ms_ = cp_ms;
        if (sync) measure_sync_ = sync->Stats();
    }

    // what the sync-only GPU did between two readings, as native_view stats
    // reports it (renderer native)
    static NativeViewStats::EmulatedGpu::Sync SyncGpu(const render::sync_gpu::SyncGpuStats& now,
                                                      const render::sync_gpu::SyncGpuStats& from) {
        const render::sync_gpu::SyncCpDelta d = render::sync_gpu::DeltaOf(now.cp, from.cp);
        NativeViewStats::EmulatedGpu::Sync s;
        s.packets = d.packets;
        for (size_t i = 0; i < d.opcodes.size() && i < 8; i++)
            s.opcodes.emplace_back(render::sync_gpu::Pm4OpcodeName(d.opcodes[i].first),
                                   d.opcodes[i].second);
        s.draws_skipped = d.draws_skipped;
        s.waits = d.waits;
        s.stalled_waits = d.stalled_waits;
        s.wait_ms = d.wait_ms;
        s.wait_max_ms = d.wait_max_ms;
        for (int b = 0; b < render::sync_gpu::kWaitBands; b++)
            s.stalled_by_band.push_back({render::sync_gpu::WaitBandName(b), d.stalled_by_band[b],
                                         d.wait_ms_by_band[b], d.polls_by_band[b]});
        for (const auto& [wait, n] : d.wait_values)
            s.wait_intervals.emplace_back(std::format("0x{:X}", wait), n);
        s.interrupts = d.interrupts;
        s.swaps = d.swaps;
        s.vblanks = now.vblanks - from.vblanks;
        s.fences = d.fences;
        s.zpd_writes = d.sample_count_writes;
        s.unknown_opcodes = d.unknown_opcodes;
        s.unknown_registers = d.unknown_registers;
        s.bad_packets = d.bad_packets;
        s.bad_addresses = d.bad_addresses;
        // since the thread started, if the first reading came before it
        if (now.thread_ms >= 0) s.thread_ms = now.thread_ms - std::max(from.thread_ms, 0.0);
        if (now.vblank_thread_ms >= 0)
            s.vblank_thread_ms = now.vblank_thread_ms - std::max(from.vblank_thread_ms, 0.0);
        return s;
    }

    // what the emulated GPU was sent, as native_view stats reports it
    static NativeViewStats::EmulatedGpu EmulatedGpu(const render::GpuSkipStats& g) {
        using G = render::GpuSkipStats;
        NativeViewStats::EmulatedGpu e;
        e.mode = render::SkipLevelName(g.level);
        e.skip_mode = g.skip_mode;
        e.skipping = g.skipping;
        e.fresh = g.fresh;
        e.frames = g.frames;
        e.frames_skipped = g.frames_skipped;
        for (int i = 0; i < G::kNumKinds; i++) {
            e.emitted.emplace_back(G::kKindNames[i], g.emitted[i]);
            e.skipped.emplace_back(G::kKindNames[i], g.skipped[i]);
        }
        e.kept_pass = g.kept_pass;
        e.kept_point_tests = g.kept_point_tests;
        e.passes_dropped = g.passes_dropped;
        return e;
    }

    // the capture's cost as native_view stats reports it, in milliseconds
    static NativeViewStats::Capture CaptureCost(const render::CaptureProfile& p) {
        using P = render::CaptureProfile;
        NativeViewStats::Capture c;
        c.frames = p.frames;
        c.captured = p.captured;
        for (int i = 0; i < P::kNumHooks; i++)
            c.hooks_ms.emplace_back(P::kHookNames[i], double(p.hook_ns[i]) / 1e6);
        c.draws = p.draws;
        c.steps = p.steps_on;
        for (int i = 0; i < P::kNumSteps; i++)
            if (p.step_calls[i]) c.steps_ms.emplace_back(P::kStepNames[i], double(p.step_ns[i]) / 1e6);
        c.counts = {{"new_shades", p.new_shades},
                    {"allocs", p.allocs},
                    {"geom_miss_bytes", p.geom_miss_bytes},
                    {"tex_decode_bytes", p.tex_decode_bytes},
                    {"bones", p.bones},
                    {"deferred_decodes", p.deferred_decodes},
                    {"deferred_decode_us", p.deferred_decode_us}};
        for (int i = 0; i < P::kNumSteps; i++)
            if (p.step_calls[i])
                c.counts.emplace_back(std::string(P::kStepNames[i]) + "_n", p.step_calls[i]);
        c.sizes = {{"rts", p.rts}, {"geoms", p.geoms}, {"texs", p.texs}, {"map_texs", p.map_texs}};
        return c;
    }

    // a kind of frame's numbers as native_view stats reports them (totals,
    // which the reply divides by the frames drawn)
    static NativeViewStats::Kind ByKind(render::FrameKind which,
                                        render::LiveViewStats::Kind& k) {
        using P = render::CaptureProfile;
        NativeViewStats::Kind out;
        out.name = render::FrameKindName(which);
        out.rendered = k.rendered;
        out.skipped_busy = k.skipped_busy;
        out.ms = std::move(k.ms);
        out.wait_ms = std::move(k.wait_ms);
        const render::GpuStats& g = k.gpu;
        out.parts_ms = {{"pre", g.pre_ms},
                        {"plan", g.plan_ms},
                        {"plan_setup", g.plan_setup_ms},
                        {"plan_walk", g.plan_walk_ms},
                        {"plan_targets", g.targets_ms},
                        {"plan_arrays", g.arrays_ms},
                        {"plan_arena", g.plan_arena_ms},
                        {"plan_reserve", g.plan_reserve_ms},
                        {"upload", g.upload_ms},
                        {"record", g.record_ms},
                        {"post_plan", g.post_plan_ms},
                        {"submit", g.submit_ms},
                        {"wait", g.wait_ms},
                        {"evict", g.evict_ms}};
        const render::GpuStats& most = k.plan_most;
        out.plan_max_ms = {{"plan", most.plan_ms},
                           {"setup", most.plan_setup_ms},
                           {"walk", most.plan_walk_ms},
                           {"targets", most.targets_ms},
                           {"arrays", most.arrays_ms},
                           {"arena", most.plan_arena_ms},
                           {"reserve", most.plan_reserve_ms},
                           {"post_plan", most.post_plan_ms}};
        out.plan_spikes = k.plan_spikes;
        out.plan_spikes_at_cut = k.plan_spikes_at_cut;
        out.counts = {{"gpu_frames", double(k.gpu_frames)},
                      {"composed", double(k.composed)},
                      {"shows_kept", double(k.shows_kept)},
                      {"draws", double(g.draws)},
                      {"world_draws", double(g.world_draws)},
                      {"passes", double(g.passes)},
                      {"pre_passes", double(g.pre_passes)},
                      {"pool_meshes", double(g.pool_meshes)},
                      {"arena_moved", double(g.arena_moved)},
                      {"arena_sent", double(g.arena_sent)},
                      {"arena_rebuilt", double(k.arena_rebuilt)},
                      {"arena_copied", double(g.arena_copied)},
                      {"mesh_bytes", double(g.mesh_bytes)},
                      {"textures_sent", double(g.textures_sent)},
                      {"texture_bytes", double(g.texture_bytes)},
                      {"bone_bytes", double(g.bone_bytes)},
                      {"pipelines_made", double(g.pipelines_made)},
                      {"buffers_made", double(g.buffers_made)},
                      {"textures_made", double(g.textures_made)},
                      {"targets_made", double(g.targets_made)},
                      {"targets_new", double(g.targets_new)},
                      {"targets_resized", double(g.targets_resized)},
                      {"targets_returning", double(g.targets_returning)},
                      {"textures_first", double(g.textures_first)},
                      {"meshes_first", double(g.meshes_first)},
                      {"arrays_grown", double(g.arrays_grown)},
                      {"arrays_mb", g.arrays_mb},
                      {"arena_new_mb", g.arena_new_mb},
                      {"reserve_grew", double(g.reserve_grew)},
                      {"evicted_meshes", double(g.evicted_meshes)},
                      {"evicted_textures", double(g.evicted_textures)},
                      {"evicted_rts", double(g.evicted_rts)},
                      {"rts_released", double(g.rts_released)},
                      {"textures_pressured", double(g.textures_pressured)},
                      {"meshes_pressured", double(g.meshes_pressured)},
                      {"resident_meshes", double(g.resident_meshes)},
                      {"resident_textures", double(g.resident_textures)},
                      {"resident_rts", double(g.resident_rts)},
                      {"meshes_by_time", double(g.meshes_by_time)},
                      {"textures_by_time", double(g.textures_by_time)},
                      {"texture_array_mb", g.texture_array_mb},
                      {"arena_mb", g.arena_mb},
                      {"rts_mb", g.rts_mb}};
        out.peak = {{"meshes", double(k.peak_meshes)},
                    {"textures", double(k.peak_textures)},
                    {"rts", double(k.peak_rts)},
                    {"meshes_by_time", double(k.peak_meshes_by_time)},
                    {"textures_by_time", double(k.peak_textures_by_time)},
                    {"texture_array_mb", k.peak_texture_array_mb},
                    {"arena_mb", k.peak_arena_mb},
                    {"rts_mb", k.peak_rts_mb}};
        for (int i = 0; i < P::kNumHooks; i++)
            out.capture_ms.emplace_back(P::kHookNames[i], double(k.cost.hook_ns[i]) / 1e6);
        out.capture_counts = {{"draws", double(k.cost.draws)},
                              {"new_shades", double(k.cost.new_shades)},
                              {"allocs", double(k.cost.allocs)},
                              {"bones", double(k.cost.bones)},
                              {"geom_miss_bytes", double(k.cost.geom_miss_bytes)},
                              {"tex_decode_bytes", double(k.cost.tex_decode_bytes)},
                              {"game_ms", double(k.cost.game_ns) / 1e6}};
        return out;
    }

    rex::Runtime* runtime_;
    rex::ui::WindowedAppContext* app_context_;
    rex::ui::Window* window_;
    const std::atomic<bool>& stopping_;
    std::mutex measure_mutex_;
    Clock::time_point measure_start_ = Clock::now();
    uint64_t measure_frame_ = 0;
    render::PassRecordingStats measure_rec_;
    render::CaptureProfile measure_profile_;
    render::GpuSkipStats measure_gpu_;
    double measure_cp_ms_ = -1;  // CpThreadMs
    // the sync-only GPU's, with renderer native
    render::sync_gpu::SyncGpuStats measure_sync_;
    // the frame cap's totals and the game's Presents so far when
    // present_stats last started over
    pacing::FrameCapStats present_cap_;
    uint64_t present_frames_ = 0;
};

bool SendAll(socket_t s, const std::string& data) {
#ifdef _WIN32
    constexpr int kFlags = 0;
#else
    // band3ctl going away mid-reply (a timeout, Ctrl-C) mustn't SIGPIPE the game
    constexpr int kFlags = MSG_NOSIGNAL;
#endif
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), kFlags);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

// waits up to kStopCheck for either socket to be readable; which one, or neither
enum class Ready { kNone, kListener, kClient, kError };
Ready WaitReadable(socket_t listener, socket_t client) {
    fd_set read;
    FD_ZERO(&read);
    FD_SET(listener, &read);
    if (client != kNoSocket) FD_SET(client, &read);
    timeval timeout{0, static_cast<long>(kStopCheck.count() * 1000)};
#ifdef _WIN32
    const int nfds = 0;  // ignored on Windows
#else
    const int nfds = std::max(listener, client) + 1;
#endif
    const int n = select(nfds, &read, nullptr, nullptr, &timeout);
    if (n < 0) return Ready::kError;
    if (n == 0) return Ready::kNone;
    if (client != kNoSocket && FD_ISSET(client, &read)) return Ready::kClient;
    return Ready::kListener;
}

class Server {
public:
    static Server& Get() {
        static Server server;
        return server;
    }

    void Start(rex::Runtime* runtime, rex::ui::WindowedAppContext* app_context,
               rex::ui::Window* window) {
        if (thread_.joinable()) return;
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            REXLOG_WARN("Test server: WSAStartup failed, the server is off");
            return;
        }
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kNoSocket) {
            REXLOG_WARN("Test server: couldn't make a socket, the server is off");
            return;
        }
#ifndef _WIN32
        // so a restarted game can take the port straight back
        int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(g_port));
        // this machine only
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listener_, 1) != 0) {
            REXLOG_WARN("Test server: port {} is in use or unavailable, the server is off",
                        g_port);
            CloseSocket(listener_);
            listener_ = kNoSocket;
            return;
        }
        target_ = std::make_unique<GameTarget>(runtime, app_context, window, stopping_);
        stopping_ = false;
        thread_ = std::thread([this] { Run(); });
        REXLOG_INFO("Test server: listening on 127.0.0.1:{}", g_port);
    }

    void Stop() {
        if (!thread_.joinable()) return;
        stopping_ = true;
        thread_.join();
        CloseSocket(listener_);
        listener_ = kNoSocket;
        target_.reset();
    }

private:
    void Run() {
        socket_t client = kNoSocket;
        std::string pending;
        while (!stopping_) {
            const Ready ready = WaitReadable(listener_, client);
            if (ready == Ready::kError) {
                REXLOG_WARN("Test server: select failed, the server stops");
                break;
            }
            if (ready == Ready::kListener) {
                socket_t accepted = accept(listener_, nullptr, nullptr);
                if (accepted == kNoSocket) continue;
                if (client != kNoSocket) {
                    SendAll(accepted, "{\"ok\":false,\"error\":\"another client is connected\"}\n");
                    CloseSocket(accepted);
                    continue;
                }
                client = accepted;
                pending.clear();
                REXLOG_INFO("Test server: client connected");
                continue;
            }
            if (ready != Ready::kClient) continue;

            char buf[1024];
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) {
                Disconnect(client);
                continue;
            }
            pending.append(buf, static_cast<size_t>(n));
            size_t newline;
            while ((newline = pending.find('\n')) != std::string::npos) {
                const std::string line = pending.substr(0, newline);
                pending.erase(0, newline + 1);
                if (!SendAll(client, RunCommand(line, *target_) + "\n")) {
                    Disconnect(client);
                    break;
                }
            }
            if (client != kNoSocket && pending.size() > kMaxLine) {
                SendAll(client, "{\"ok\":false,\"error\":\"line too long\"}\n");
                Disconnect(client);
            }
        }
        if (client != kNoSocket) Disconnect(client);
    }

    // a client that goes away can't leave anything held down
    void Disconnect(socket_t& client) {
        CloseSocket(client);
        client = kNoSocket;
        ReleaseAllPlayers(*target_);
        REXLOG_INFO("Test server: client disconnected");
    }

    socket_t listener_ = kNoSocket;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    std::unique_ptr<GameTarget> target_;
};

}

bool Enabled() { return g_port > 0; }

void Init() {
    g_port = REXCVAR_GET(test_port);
    if (!Enabled()) return;
    // the commands play the virtual instrument, as player 1
    rex::cvar::SetFlagByName("virtual_instrument", "true");
    rex::cvar::SetFlagByName("virtual_instrument_player", "1");
}

void StartServer(rex::Runtime* runtime, rex::ui::WindowedAppContext* app_context,
                 rex::ui::Window* window) {
    if (Enabled()) Server::Get().Start(runtime, app_context, window);
}

void StopServer() { Server::Get().Stop(); }

}
