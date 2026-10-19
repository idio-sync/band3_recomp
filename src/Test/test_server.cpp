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
#include <cctype>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
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
#include "src/Content/game_song_list.h"
#include "src/Content/live_content.h"
#include "src/Content/package_songs.h"
#include "src/Video/music_video.h"
#include "src/Video/video_jobs.h"
#include "src/Hooks/frame_pacing.h"
#include "src/Hooks/mouse_hover.h"
#include "src/Input/input_lock.h"
#include "src/Input/input_system.h"
#include "src/Input/keyboard_search_driver.h"
#include "src/Input/midi_keys_driver.h"
#include "src/Input/mouse_menus_driver.h"
#include "src/Input/ui_round_trip.h"
#include "src/Input/virtual_instrument.h"
#include "src/Input/xinput_state.h"
#include "src/Lights/lights.h"
#include "src/Net/home_assistant.h"
#include "src/Net/liveless_rooms.h"
#include "src/Net/online_hooks.h"
#include "src/Net/port_mapping.h"
#include "src/Render/capture_file.h"
#include "src/Render/frame_compose.h"
#include "src/Render/gpu_skip.h"
#include "src/Render/gpu_view.h"
#include "src/Render/native_view.h"
#include "src/Render/png_writer.h"
#include "src/Render/post_model.h"
#include "src/Render/scene_capture.h"
#include "src/Render/sync_gpu/native_only.h"
#include "src/Render/sync_gpu/sync_graphics_system.h"
#include "src/Render/sync_gpu/sync_monitor.h"
#include "src/Render/vr_pose.h"
#include "src/Render/vr_venue.h"
#include "src/Render/xr_present.h"
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

// `capture`'s `vr`: the layout VR draws the captured frame with
// (xr::LayoutFor), from the headset's last views or the stand-in's: whether
// the eyes are through the game camera's window or black, the fade, the
// game's field of view in degrees, the window's size and pose in LOCAL space
// (metres), the game's picture's share of it each way (`picture_scale`, under
// 1 where a song's squeeze is capped), where the panel goes, the dark's quads,
// the eyes' picture size, and the window's, the picture's (`picture_px`) and
// the panel's corners in the left eye's picture (pixels from
// its top left: top left, top right, bottom right, bottom left; [] for no
// window, null for a corner behind the eye), the tracks' (`tracks`, below);
// with `centre`, the same corners in its picture (`centre`: .gpu.C.png's eye)
std::string VrLayoutJson(const render::vr::FrameLayout& l, const render::xr::FrameViews& views,
                         bool headset, float fade, uint32_t eye_w, uint32_t eye_h,
                         const render::xr::FrameViews* centre, uint32_t centre_size) {
    auto pose = [](const render::vr::XrPose& p) {
        return std::format("{{\"p\":[{:.4f},{:.4f},{:.4f}],\"q\":[{:.4f},{:.4f},{:.4f},{:.4f}]}}",
                           p.p[0], p.p[1], p.p[2], p.q[0], p.q[1], p.q[2], p.q[3]);
    };
    auto pixels = [&](const render::vr::HudQuad& q, const render::xr::FrameViews& v,
                      uint32_t w, uint32_t h) {
        float c[4][3];
        render::vr::QuadCorners(q, c);
        std::string out = "[";
        for (int i = 0; i < 4; i++) {
            float x = 0, y = 0;
            out += i ? "," : "";
            if (render::vr::EyePixel(v.pose[0], v.fov[0], w, h, c[i], x, y))
                out += std::format("[{:.2f},{:.2f}]", x, y);
            else
                out += "null";
        }
        return out + "]";
    };
    const render::vr::WindowFrame& w = l.window_frame;
    render::vr::HudQuad window;
    window.pose = w.pose;
    window.size[0] = w.size[0];
    window.size[1] = w.size[1];
    // the game's picture in it: the window, or its centre where a song's
    // squeeze is capped (`picture_scale`, vr_window_max_squeeze)
    render::vr::HudQuad picture = window;
    picture.size[0] *= l.picture;
    picture.size[1] *= l.picture;
    const char* hud = !l.hud ? "off" : l.hud_in_window ? "window" : "own";
    // Each track (vr_track_3d): a quad on it, its edges from where the game's
    // picture cuts it below to 60 units along it (the near half of what the
    // game shows), then its strikeline's middle; in the left eye's picture
    // (null behind the eye) and in the HUD panel's, the game's picture at
    // the eyes' size, through the track camera's captured matrix and viewport
    std::string tracks = "[";
    for (int t = 0; t < l.track_count; t++) {
        const render::vr::TrackPlace& p = l.tracks[t];
        const render::Mat4& vp = p.cam.view_proj;
        const float near_y = -(vp.m[3][1] + vp.m[3][3]) / (vp.m[1][1] + vp.m[1][3]);
        const float pts[5][3] = {
            {-9.8f, near_y, 0}, {9.8f, near_y, 0}, {9.8f, 60, 0}, {-9.8f, 60, 0}, {0, 0, 0}};
        std::string eye_px = "[", panel_px = "[";
        for (int k = 0; k < 5; k++) {
            float local[3], x = 0, y = 0, c[4];
            render::vr::TrackToLocal(p, l.track_scale, pts[k], local);
            eye_px += k ? "," : "";
            if (render::vr::EyePixel(views.pose[0], views.fov[0], eye_w, eye_h, local, x, y))
                eye_px += std::format("[{:.2f},{:.2f}]", x, y);
            else
                eye_px += "null";
            render::freecam::Transform(pts[k], vp, c);
            const float sx = p.cam.target_w ? float(eye_w) / float(p.cam.target_w) : 1;
            const float sy = p.cam.target_h ? float(eye_h) / float(p.cam.target_h) : 1;
            const float* v = p.cam.viewport;
            panel_px += std::format("{}[{:.2f},{:.2f}]", k ? "," : "",
                                    (v[0] + (c[0] / c[3] + 1) / 2 * v[2]) * sx,
                                    (v[1] + (1 - c[1] / c[3]) / 2 * v[3]) * sy);
        }
        tracks += std::format("{}{{\"cam\":\"{:08X}\",\"eye_px\":{}],\"panel_px\":{}]}}",
                              t ? "," : "", p.cam.cam, eye_px, panel_px);
    }
    tracks += "]";
    std::string centre_json;
    if (centre)
        centre_json = std::format(
            ",\"centre\":{{\"eye_size\":[{},{}],\"window_px\":{},\"picture_px\":{},"
            "\"hud_px\":{}}}",
            centre_size, centre_size,
            l.window ? pixels(window, *centre, centre_size, centre_size) : "[]",
            l.window ? pixels(picture, *centre, centre_size, centre_size) : "[]",
            pixels(l.hud_quad, *centre, centre_size, centre_size));
    return std::format(
        "{{\"source\":\"{}\",\"hud_boundary\":{},\"window\":{},\"eyes_black\":{},\"fade\":{:.3f},"
        "\"hfov\":{:.2f},\"vfov\":{:.2f},\"window_deg\":{:.2f},\"magnify\":{:.3f},"
        "\"picture_scale\":{:.4f},"
        "\"distance\":{:.3f},\"window_size\":[{:.4f},{:.4f}],"
        "\"window_pose\":{},\"head\":{},\"hud\":\"{}\",\"hud_size\":[{:.4f},{:.4f}],"
        "\"hud_pose\":{},\"frame_quads\":{},\"frame_layers\":{},\"dim\":{:.3f},"
        "\"eye_size\":[{},{}],\"window_px\":{},\"picture_px\":{},\"hud_px\":{},"
        "\"tracks\":{}{}}}",
        headset ? "headset" : "stand_in", l.hud_boundary, l.window ? "true" : "false",
        l.eyes_black ? "true" : "false", fade, l.game_fov.hfov * 57.2957795f,
        l.game_fov.vfov * 57.2957795f,
        w.distance > 0 ? 2 * std::atan2(w.size[0] / 2, w.distance) * 57.2957795f : 0.0f,
        l.magnify, l.picture, w.distance, w.size[0], w.size[1], pose(w.pose),
        pose(views.head), hud, l.hud_quad.size[0], l.hud_quad.size[1], pose(l.hud_quad.pose),
        l.frame_quads, l.frame_layers ? "true" : "false", l.dim, eye_w, eye_h,
        l.window ? pixels(window, views, eye_w, eye_h) : "[]",
        l.window ? pixels(picture, views, eye_w, eye_h) : "[]",
        pixels(l.hud_quad, views, eye_w, eye_h), tracks, centre_json);
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
                state.mics.push_back({slot.device, slot.connected, slot.bytes_fed, slot.monitored,
                                      slot.monitor_gain});
            }
        }
        state.rooms_state = rooms::StateName(rooms::GetStatus().state);
        state.port_mapping_state = port_mapping::StateName(port_mapping::GetStatus().state);
        state.ha_state = ha::StateName();
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
        options.hardware_sampling = REXCVAR_GET(native_view_hardware_sampling);
        options.shader_variants = REXCVAR_GET(native_view_shader_variants);
        options.instancing = REXCVAR_GET(native_view_instancing);
        options.hoist_passes = REXCVAR_GET(native_view_hoist_passes);
        options.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
        options.post_only = render::post::PostOnlyFor(REXCVAR_GET(native_post_effects));
        options.velocity = REXCVAR_GET(native_post_effects);
        options.bc_textures = REXCVAR_GET(native_bc_textures);
        options.r8_textures = REXCVAR_GET(native_r8_textures);
        options.inline_mips = REXCVAR_GET(native_view_inline_mips);
        options.premake_targets = REXCVAR_GET(native_view_premake_targets);
        options.premake_arrays = REXCVAR_GET(native_view_premake_arrays);
        options.submit_points = uint32_t(REXCVAR_GET(native_view_submit_points));
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

    // the median of three
    static double Mid(double a, double b, double c) {
        return std::max(std::min(a, b), std::min(std::max(a, b), c));
    }

    // Three rounds' GpuStats of one picture as `eye_cost` has them, each the
    // median of its three: the CPU's ms to the picture read back and its
    // parts (planning, upload, recording, submitting), what it drew, and with
    // timestamps the GPU's ms by part (gpu_timing_model.h), the readback left
    // out (`gpu_ms`): the world, the texture passes (outfits, heads, the
    // crowd's impostors, NgLight's shadow: pass_other; self-shadow maps;
    // spotlights' with their blurs), their mips, post-processing with the
    // resolve's copies, the eyes' own overlay draws (the tracks and the
    // dark), the gamma pass, the upload, and the GPU idle between submissions
    static std::string EyeStatsJson(const render::GpuStats* s) {
        using render::GpuStats;
        namespace t = render::gpu_timing;
        auto mid = [&](auto get) { return Mid(double(get(s[0])), double(get(s[1])), double(get(s[2]))); };
        auto gpu = [&](std::initializer_list<int> parts) {
            return mid([&](const GpuStats& g) {
                double sum = 0;
                for (int p : parts) sum += g.gpu_ms[p];
                return g.gpu_timed ? sum : -1.0;
            });
        };
        const double total = mid([](const GpuStats& g) {
            return g.gpu_timed ? g.gpu_total_ms - g.gpu_ms[t::kReadback] : -1.0;
        });
        return std::format(
            "{{\"ms\":{:.3f},\"plan_ms\":{:.3f},\"upload_ms\":{:.3f},\"record_ms\":{:.3f},"
            "\"record_passes_ms\":{:.3f},"
            "\"submit_ms\":{:.3f},\"acquire_ms\":{:.3f},\"first_draw_ms\":{:.3f},"
            "\"uniform_slow_ms\":{:.3f},\"draw_slow_ms\":{:.3f},\"submits\":{:.0f},"
            "\"draws\":{:.0f},\"world_draws\":{:.0f},"
            "\"instanced_draws\":{:.0f},\"passes\":{:.0f},\"passes_kept\":{:.0f},"
            "\"passes_shared\":{:.0f},\"passes_eye\":{:.0f},"
            "\"gpu_ms\":{:.3f},\"gpu\":{{\"world\":{:.3f},\"pass_other\":{:.3f},"
            "\"pass_shadow\":{:.3f},\"pass_spot\":{:.3f},\"mips\":{:.3f},\"post\":{:.3f},"
            "\"overlay\":{:.3f},\"gamma\":{:.3f},\"upload\":{:.3f},\"idle\":{:.3f}}}}}",
            mid([](const GpuStats& g) { return g.ms; }),
            mid([](const GpuStats& g) { return g.plan_ms; }),
            mid([](const GpuStats& g) { return g.upload_ms; }),
            mid([](const GpuStats& g) { return g.record_ms; }),
            mid([](const GpuStats& g) { return g.record_passes_ms; }),
            mid([](const GpuStats& g) { return g.submit_ms; }),
            mid([](const GpuStats& g) { return g.acquire_ms; }),
            mid([](const GpuStats& g) { return g.first_draw_ms; }),
            mid([](const GpuStats& g) { return g.uniform_slow_ms; }),
            mid([](const GpuStats& g) { return g.draw_slow_ms; }),
            mid([](const GpuStats& g) { return g.submits; }),
            mid([](const GpuStats& g) { return g.draws; }),
            mid([](const GpuStats& g) { return g.world_draws; }),
            mid([](const GpuStats& g) { return g.instanced_draws; }),
            mid([](const GpuStats& g) { return g.passes; }),
            mid([](const GpuStats& g) { return g.passes_kept; }),
            mid([](const GpuStats& g) { return g.passes_shared; }),
            mid([](const GpuStats& g) { return g.passes_eye; }), total, gpu({t::kWorld}),
            gpu({t::kPassOther}), gpu({t::kPassShadow}), gpu({t::kPassSpot, t::kPassBlur}),
            gpu({t::kMips}),
            gpu({t::kCopies, t::kVelocity, t::kDof, t::kBloom, t::kComposite}),
            gpu({t::kOverlay, t::kOverlayStart, t::kOverlayResolve}), gpu({t::kGamma}),
            gpu({t::kUpload}), gpu({t::kIdle}));
    }

    // The VR pictures of `frame` (GpuCapture's, with `vr` on) and its layout's
    // JSON, from the last VR frame's views or the stand-in head. `loaded`: a
    // capture read from a file (vr_eyes), always the stand-in's with its eyes
    // 64 mm apart, laid out as in a song, a fixed viewpoint seen from that
    // anchor.
    std::string VrPictures(const render::FrameCapture& frame, const std::string& file,
                           const render::RasterOptions& options,
                           const render::vr::Anchor* loaded) {
        std::string vr;
        render::xr::FrameViews views;
        float fade = 0;
        uint32_t eye_w = 0, eye_h = 0;
        const bool headset =
            !loaded && render::xr::Session::Get().LastFrame(views, fade, eye_w, eye_h);
        // the stand-in's and the centre eye's: 60 degrees each way, wider
        // than vr_window_width's default 100 across
        constexpr float kHalf = 1.0471976f;
        if (!headset) {
            for (int e = 0; e < 2; e++) {
                views.pose[e] = {};
                views.fov[e] = {-kHalf, kHalf, kHalf, -kHalf};
                // a loaded capture's eyes a typical IPD apart (64 mm), so
                // the two differ as a headset's do
                if (loaded) views.pose[e].p[0] = e ? 0.032f : -0.032f;
            }
            views.have_head = true;
            views.head = {};
            eye_w = eye_h = 1440;
        }
        const bool in_song = loaded || (render::InSong() && render::SongScreen());
        const render::vr::FrameLayout layout =
            render::xr::LayoutFor(frame, views, in_song, nullptr, loaded);
        constexpr uint32_t kCentreSize = 1440;
        render::xr::FrameViews centre = views;
        const bool with_centre = layout.window;
        if (with_centre) {
            for (int e = 0; e < 2; e++) {
                centre.pose[e].p[0] = views.head.p[0];
                centre.pose[e].p[1] = views.head.p[1];
                centre.pose[e].p[2] = views.head.p[2];
                for (int k = 0; k < 4; k++) centre.pose[e].q[k] = layout.window_frame.pose.q[k];
                centre.fov[e] = {-kHalf, kHalf, kHalf, -kHalf};
            }
        }
        vr = VrLayoutJson(layout, views, headset, fade, eye_w, eye_h,
                          with_centre ? &centre : nullptr, kCentreSize);
        render::RasterOptions eo = options;
        // at vr_eye_scale or vr_window_eye_scale of the headset's size, as
        // VrFrame draws them (their PNGs that size)
        const float eye_scale =
            render::vr::EyeScaleOf(layout, float(REXCVAR_GET(vr_eye_scale)),
                                   float(REXCVAR_GET(vr_window_eye_scale)));
        eo.width = render::vr::EyeDrawSize(eye_w, eye_scale);
        eo.height = render::vr::EyeDrawSize(eye_h, eye_scale);
        render::ScaleForPicture(eo);
        eo.msaa = 1;
        eo.velocity = false;
        eo.post_only = render::post::kPostXfm | render::post::kPostBloom |
                       render::post::kPostGlare;
        eo.part = render::BackBufferPart::kEyes;
        eo.hud_from = layout.hud_boundary;
        render::vr::EyeCamsOf(layout, eo.eye_cams, render::RasterOptions::kEyeCams);
        // Both eyes as VrFrame draws them with vr_shared_eyes, where no
        // session holds the eyes' outputs: from the capture itself in one go
        // (RenderEyesToOutputs into them), waited for inside (`wait`, for
        // the timings) or, as VrFrame, by polling OutputDone after; the
        // overrides' making into `make_ms`. False where VrFrame draws them
        // one at a time.
        auto both_eyes = [&](const render::RasterOptions& o, render::GpuStats gs[2],
                             render::vr::EyeOverride ov[2], double* make_ms, bool wait) {
            if (headset || layout.eyes_black || !REXCVAR_GET(vr_shared_eyes)) return false;
            const auto from = std::chrono::steady_clock::now();
            const bool made = render::vr::EyeOverrideOf(frame, layout.eyes[0], ov[0]) &&
                              render::vr::EyeOverrideOf(frame, layout.eyes[1], ov[1]);
            if (make_ms)
                *make_ms = std::chrono::duration<double, std::milli>(
                               std::chrono::steady_clock::now() - from)
                               .count();
            if (!made || !render::GpuRenderer::CanRenderEyes(frame, o)) return false;
            render::RasterOptions waited = o;
            waited.gpu_no_wait = !wait;
            const render::vr::EyeOverride* const eyes[2] = {&ov[0], &ov[1]};
            const int slots[2] = {render::GpuRenderer::EyeSlot(0), render::GpuRenderer::EyeSlot(1)};
            render::GpuOutput out[2];
            if (!render::GpuRenderer::Get().RenderEyesToOutputs(frame, waited, eyes, slots, out,
                                                                gs))
                return false;
            for (int e = 0; !wait && e < 2; e++)
                while (!render::GpuRenderer::Get().OutputDone(slots[e], &gs[e]))
                    std::this_thread::sleep_for(std::chrono::milliseconds(1));
            return true;
        };
        bool together = false;
        {
            render::GpuStats gs[2];
            render::vr::EyeOverride ov[2];
            together = both_eyes(eo, gs, ov, nullptr, false);
            for (int e = 0; together && e < 2; e++) {
                std::vector<uint32_t> eye_rgba;
                uint32_t w = 0, h = 0;
                if (render::GpuRenderer::Get().DownloadOutput(render::GpuRenderer::EyeSlot(e),
                                                              eye_rgba, w, h))
                    render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                      (file + (e ? ".gpu.R.png" : ".gpu.L.png")))
                                         .string(),
                                     eye_rgba, w, h);
            }
        }
        for (int e = 0; e < 2 && !together; e++) {
            std::vector<uint32_t> eye_rgba;
            render::GpuStats eye_stats;
            bool drew = layout.eyes_black;
            if (layout.eyes_black) {
                eye_rgba.assign(size_t(eo.width) * eo.height, 0xFF000000u);
            } else {
                // as VrFrame draws them: from the capture itself where the
                // eye can be (vr_shared_eyes), else RePose's copy; the right
                // keeping the left's shared passes
                render::RasterOptions eye_o = eo;
                render::vr::EyeOverride ov;
                render::FrameCapture eye_frame;
                const bool shared = REXCVAR_GET(vr_shared_eyes);
                if (shared && render::vr::EyeOverrideOf(frame, layout.eyes[e], ov)) {
                    eye_o.eye_override = &ov;
                    if (ov.ok) eye_o.eye_view_proj = ov.swap[0].from;
                } else {
                    eye_o.eye_view_proj =
                        render::vr::RePose(frame, layout.eyes[e], eye_frame).view_proj;
                }
                eye_o.keep_eye_passes = e == 1 && shared;
                drew = render::GpuRenderer::Get().RenderFrame(
                    eye_o.eye_override ? frame : eye_frame, eye_o, eye_rgba, eye_stats);
            }
            if (drew)
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + (e ? ".gpu.R.png" : ".gpu.L.png")))
                                     .string(),
                                 eye_rgba, eo.width, eo.height);
        }
        // the world's draws alone: the venue the window shows
        const uint32_t no_cams[render::RasterOptions::kEyeCams] = {};
        // With the tracks in the eyes, what they and the eyes' dark cost
        // the left eye: its capture re-posed and drawn with them and
        // without, by turns, GPU timestamps on, the first round left out;
        // the medians of three (RePose's ms, the CPU's to the picture
        // read back, the GPU's), as `vr`'s track_cost
        if (layout.track_count > 0 && !layout.eyes_black) {
            render::RasterOptions with = eo;
            with.gpu_timestamps = true;
            render::RasterOptions without = with;
            std::copy(std::begin(no_cams), std::end(no_cams), without.eye_cams);
            render::vr::Eye plain = layout.eyes[0];
            plain.track_count = 0;
            plain.dark_count = 0;
            double took[2][3][3] = {};  // with, without; repose, cpu, gpu; round
            for (int round = 0; round < 4; round++) {
                for (int v = 0; v < 2; v++) {
                    const auto from = std::chrono::steady_clock::now();
                    render::FrameCapture eye_frame;
                    render::vr::RePose(frame, v ? plain : layout.eyes[0], eye_frame);
                    const double repose = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - from)
                                              .count();
                    std::vector<uint32_t> px;
                    render::GpuStats gs;
                    render::GpuRenderer::Get().RenderFrame(eye_frame, v ? without : with, px,
                                                          gs);
                    if (round == 0) continue;
                    took[v][0][round - 1] = repose;
                    took[v][1][round - 1] = gs.ms;
                    took[v][2][round - 1] = gs.gpu_timed ? gs.gpu_total_ms : -1;
                }
            }
            auto median = [](double* x) {
                std::sort(x, x + 3);
                return x[1];
            };
            std::string cost = ",\"track_cost\":{";
            for (int v = 0; v < 2; v++)
                cost += std::format("{}\"{}\":{{\"repose_ms\":{:.3f},\"ms\":{:.3f},"
                                    "\"gpu_ms\":{:.3f}}}",
                                    v ? "," : "", v ? "without" : "with", median(took[v][0]),
                                    median(took[v][1]), median(took[v][2]));
            vr.insert(vr.size() - 1, cost + "}");
        }
        // What the left eye costs as drawn (`eye_cost`), timestamps on, at
        // the eyes' size and at the Steam Frame's (kFrameEye, `frame`), both
        // at the eye scale (vr_eye_scale, vr_window_eye_scale), each
        // the medians of three rounds after one left out: RePose's ms, then
        // the eye's frame drawn (`first`) and at once again (`second`, as the
        // right eye's would be: what drawing the eye-independent texture
        // passes once would save), each EyeStatsJson's; and the crowd draws
        // it left out near a fixed viewpoint. Where the eyes are
        // (`viewpoint`, `anchor`: x, y, z, yaw in degrees) and the song's
        // venue as the fixed viewpoints found it (`venue`, vr_venue.h's
        // VenueRef; a loaded capture's own)
        {
            std::string extra;
            // into the left eye's output as VrFrame draws it, waited for
            // (gpu_no_wait off), when no session uses it; else read back
            auto draw_eye = [&](const render::FrameCapture& f, const render::RasterOptions& o,
                                render::GpuStats& gs) {
                if (headset) {
                    std::vector<uint32_t> px;
                    return render::GpuRenderer::Get().RenderFrame(f, o, px, gs);
                }
                render::RasterOptions waited = o;
                waited.gpu_no_wait = false;
                render::GpuOutput output;
                return render::GpuRenderer::Get().RenderFrameToOutput(
                    f, waited, render::GpuRenderer::EyeSlot(0), output, gs);
            };
            if (!layout.eyes_black) {
                constexpr uint32_t kFrameEye = 2644;
                uint32_t cleared = 0;
                std::string sizes[2];
                for (int z = 0; z < 2; z++) {
                    render::RasterOptions timed = eo;
                    timed.gpu_timestamps = true;
                    if (z) {
                        timed.width = timed.height = render::vr::EyeDrawSize(kFrameEye, eye_scale);
                        render::ScaleForPicture(timed);
                    }
                    double repose[3] = {};
                    render::GpuStats first[3], second[3];
                    for (int round = 0; round < 4; round++) {
                        // both eyes in one go, as VrFrame draws them: the
                        // first's stats (the plan, upload and its own
                        // recording) as `first`, the second's as `second`,
                        // `repose_ms` half the overrides' making
                        {
                            render::GpuStats pair[2];
                            render::vr::EyeOverride ov[2];
                            double make_ms = 0;
                            if (both_eyes(timed, pair, ov, &make_ms, true)) {
                                together = true;
                                if (round == 0) continue;
                                repose[round - 1] = make_ms / 2;
                                first[round - 1] = std::move(pair[0]);
                                second[round - 1] = std::move(pair[1]);
                                continue;
                            }
                        }
                        // else as VrFrame: from the capture itself where it can
                        // (vr_shared_eyes; `repose_ms` is then the
                        // override's making), else RePose's copy
                        const bool shared = REXCVAR_GET(vr_shared_eyes);
                        const auto from = std::chrono::steady_clock::now();
                        render::vr::EyeOverride ov;
                        render::FrameCapture eye_frame;
                        render::RasterOptions first_o = timed;
                        if (shared && render::vr::EyeOverrideOf(frame, layout.eyes[0], ov)) {
                            first_o.eye_override = &ov;
                            if (ov.ok) first_o.eye_view_proj = ov.swap[0].from;
                        } else {
                            first_o.eye_view_proj =
                                render::vr::RePose(frame, layout.eyes[0], eye_frame).view_proj;
                        }
                        const double ms = std::chrono::duration<double, std::milli>(
                                              std::chrono::steady_clock::now() - from)
                                              .count();
                        // the second as the right eye's, keeping the passes
                        // the first drew that neither sees (vr_shared_eyes)
                        render::RasterOptions again = first_o;
                        again.keep_eye_passes = shared;
                        const render::FrameCapture& drawn = first_o.eye_override ? frame : eye_frame;
                        render::GpuStats gs[2];
                        draw_eye(drawn, first_o, gs[0]);
                        draw_eye(drawn, again, gs[1]);
                        if (round == 0) continue;
                        repose[round - 1] = ms;
                        first[round - 1] = std::move(gs[0]);
                        second[round - 1] = std::move(gs[1]);
                    }
                    if (!z) {
                        render::FrameCapture copy;
                        cleared = render::vr::RePose(frame, layout.eyes[0], copy).crowd_cleared;
                    }
                    sizes[z] = std::format(
                        "\"size\":[{},{}],\"together\":{},\"repose_ms\":{:.3f},\"first\":{},"
                        "\"second\":{}",
                        timed.width, timed.height, together ? "true" : "false",
                        Mid(repose[0], repose[1], repose[2]), EyeStatsJson(first),
                        EyeStatsJson(second));
                    // as before: the first picture's CPU and GPU at the eyes' size
                    if (!z)
                        sizes[z] += std::format(
                            ",\"ms\":{:.3f},\"gpu_ms\":{:.3f}",
                            Mid(first[0].ms, first[1].ms, first[2].ms),
                            Mid(first[0].gpu_timed ? first[0].gpu_total_ms : -1,
                                first[1].gpu_timed ? first[1].gpu_total_ms : -1,
                                first[2].gpu_timed ? first[2].gpu_total_ms : -1));
                }
                extra += std::format(",\"eye_cost\":{{{},\"draws\":{},\"crowd_cleared\":{},"
                                     "\"frame\":{{{}}}}}",
                                     sizes[0], frame.draws.size(), cleared, sizes[1]);
            }
            const render::vr::Anchor& a = layout.anchor;
            extra += std::format(",\"viewpoint\":\"{}\",\"anchor\":[{:.1f},{:.1f},{:.1f},{:.1f}]",
                                 REXCVAR_GET(vr_viewpoint), a.pos[0], a.pos[1], a.pos[2],
                                 a.yaw * 57.2957795f);
            const render::vr::VenueRef v =
                loaded ? render::vr::VenueRefOf(frame) : render::xr::CurrentVenue();
            std::string members = "[";
            for (int m = 0; m < v.members; m++)
                members += std::format("{}[{:.1f},{:.1f},{:.1f}]", m ? "," : "",
                                       v.member[m][0], v.member[m][1], v.member[m][2]);
            extra += std::format(
                ",\"venue\":{{\"members\":{}],\"centre\":[{:.1f},{:.1f},{:.1f}],"
                "\"toward\":[{:.3f},{:.3f}],\"toward_crowd\":{},\"band_front\":{:.1f},"
                "\"band_back\":{:.1f},\"band_half\":{:.1f},\"crowd\":{},\"crowd_ahead\":{},"
                "\"crowd_front\":{:.1f},\"crowd_back\":{:.1f},\"audience_floor\":{:.1f}}}",
                members, v.centre[0], v.centre[1], v.centre[2], v.toward[0], v.toward[1],
                v.toward_crowd ? "true" : "false", v.band_front, v.band_back, v.band_half,
                v.crowd, v.crowd_ahead, v.crowd_front, v.crowd_back, v.audience_floor);
            vr.insert(vr.size() - 1, extra);
        }
        // and beside the pictures, for tools/vr_window_check.py
        std::ofstream(rex::filesystem::GetExecutableFolder() / "screenshots" /
                      (file + ".vr.json"))
            << vr;
        if (with_centre) {
            render::RasterOptions co = eo;
            co.width = co.height = kCentreSize;
            std::copy(std::begin(no_cams), std::end(no_cams), co.eye_cams);
            render::ScaleForPicture(co);
            render::vr::FrameLayout cl = render::xr::LayoutFor(frame, centre, in_song, nullptr,
                                                               loaded);
            cl.eyes[0].track_count = 0;
            cl.eyes[0].dark_count = 0;
            render::FrameCapture eye_frame;
            render::vr::RePose(frame, cl.eyes[0], eye_frame);
            std::vector<uint32_t> centre_rgba;
            render::GpuStats centre_stats;
            if (render::GpuRenderer::Get().RenderFrame(eye_frame, co, centre_rgba,
                                                       centre_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.C.png"))
                                     .string(),
                                 centre_rgba, kCentreSize, kCentreSize);
        }
        if (layout.window) {
            render::RasterOptions wo = eo;
            wo.width = options.width;
            wo.height = options.height;
            std::copy(std::begin(no_cams), std::end(no_cams), wo.eye_cams);
            render::ScaleForPicture(wo);
            std::vector<uint32_t> world_rgba;
            render::GpuStats world_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, wo, world_rgba, world_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.eyes.png"))
                                     .string(),
                                 world_rgba, wo.width, wo.height);
        }
        // and with vr_hud, the HUD panel as VrFrame draws it, at the eyes'
        // size and the window's MSAA: <name>.gpu.hud.png, its colour
        // premultiplied and its alpha the coverage the quad layer blends
        // by (native_view_replay --overlay-only --diff sets the CPU's
        // against it)
        if (REXCVAR_GET(vr_hud)) {
            render::RasterOptions ho = eo;
            ho.part = render::BackBufferPart::kHud;
            ho.msaa = options.msaa;
            ho.width = eye_w;
            ho.height = eye_h;
            render::ScaleForPicture(ho);
            std::vector<uint32_t> hud_rgba;
            render::GpuStats hud_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, ho, hud_rgba, hud_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.hud.png"))
                                     .string(),
                                 hud_rgba, eye_w, eye_h);
        }
        return vr;
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
        render::RasterOptions options = CaptureOptions(out.screenshot.width, out.screenshot.height);
        std::vector<uint32_t> rgba;
        render::GpuStats stats;
        if (!render::GpuRenderer::Get().RenderFrame(frame, options, rgba, stats)) {
            out.gpu_error = "the GPU didn't draw the frame (see the log)";
            return;
        }
        GpuCaptureRest(frame, file, options, rgba, stats, out);
    }

    // `vr_eyes`: a capture `capture` saved (screenshots/<name>.cap) drawn
    // as `capture` draws its VR pictures (VrPictures), into <out>.gpu.L.png,
    // .gpu.R.png, <out>.vr.json and the rest, laid out as in a song with the
    // settings as they stand; a fixed viewpoint seen from `anchor`
    // ("x,y,z,yaw") or, without one, the place PresetAnchor finds in the
    // capture's own venue. The same capture drawn again by a later build
    // gives the same eyes, to compare.
    std::string VrEyes(const std::string& name, const std::string& out_name,
                       const std::string& anchor, std::string& vr_json) override {
        if (!REXCVAR_GET(vr)) return "vr is off";
        if (REXCVAR_GET(native_view_backend) != "gpu")
            return "native_view_backend is " + REXCVAR_GET(native_view_backend);
        bool ready = false;
        OnUIThread([&] { ready = render::GpuRenderer::Get().Init(); });
        if (!ready) return "no GPU device for the native view (see the log)";
        const std::filesystem::path path =
            rex::filesystem::GetExecutableFolder() / "screenshots" / (name + ".cap");
        auto frame = render::LoadCapture(path.string());
        if (!frame) return "couldn't read " + path.string();
        render::vr::Anchor at;
        render::vr::ViewMode mode = render::vr::ViewMode::kGameCamera;
        render::vr::ParseViewMode(REXCVAR_GET(vr_viewpoint), mode);
        if (!anchor.empty()) {
            if (!render::vr::ParseAnchor(anchor, at)) return "an anchor is x,y,z,yaw";
        } else if (mode != render::vr::ViewMode::kGameCamera) {
            render::vr::ViewOffset off;
            render::vr::ParseViewOffset(REXCVAR_GET(vr_viewpoint_offset), off);
            const float units = render::vr::kUnitsPerMetre * float(REXCVAR_GET(vr_world_scale));
            if (!render::vr::PresetAnchor(mode, render::vr::VenueRefOf(*frame), off, units, at))
                return "no band in " + name + "'s capture to place " +
                       REXCVAR_GET(vr_viewpoint) + " by; give an anchor";
        }
        vr_json = VrPictures(*frame, out_name, CaptureOptions(1280, 720), &at);
        return {};
    }

    // what `capture` draws its GPU pictures with: the window's settings, at
    // its picture's size or, with renderer native, the game's (1280x720)
    static render::RasterOptions CaptureOptions(uint32_t width, uint32_t height) {
        render::RasterOptions options;
        options.width = width;
        options.height = height;
        // renderer native: the screenshot is the native renderer's, at the
        // window's size, so this one is at the game's, as with the emulated
        // GPU (the window's size is .gpu.presented.png's)
        if (render::sync_gpu::NativeOnly()) {
            options.width = 1280;
            options.height = 720;
        }
        options.normal_maps = REXCVAR_GET(native_view_normal_maps);
        options.filtering = REXCVAR_GET(native_view_texture_filtering);
        options.hardware_sampling = REXCVAR_GET(native_view_hardware_sampling);
        options.shader_variants = REXCVAR_GET(native_view_shader_variants);
        options.instancing = REXCVAR_GET(native_view_instancing);
        options.hoist_passes = REXCVAR_GET(native_view_hoist_passes);
        options.msaa = uint32_t(REXCVAR_GET(native_view_msaa));
        options.post_only = render::post::PostOnlyFor(REXCVAR_GET(native_post_effects));
        options.velocity = REXCVAR_GET(native_post_effects);
        options.bc_textures = REXCVAR_GET(native_bc_textures);
        options.r8_textures = REXCVAR_GET(native_r8_textures);
        options.inline_mips = REXCVAR_GET(native_view_inline_mips);
        options.premake_targets = REXCVAR_GET(native_view_premake_targets);
        options.premake_arrays = REXCVAR_GET(native_view_premake_arrays);
        options.submit_points = uint32_t(REXCVAR_GET(native_view_submit_points));
        options.clock_keep = render::InSong();  // as NativePicture's
        // keeps the game's earlier frames' textures as the native renderer's
        // own frames do, so the CPU's drawing of the capture, which draws
        // them all, checks what was kept
        options.keep_carried = REXCVAR_GET(native_keep_carried_passes);
        return options;
    }

    // the rest of GpuCapture, after the frame's first drawing (`rgba`, `stats`)
    void GpuCaptureRest(const render::FrameCapture& frame, const std::string& file,
                        render::RasterOptions options, std::vector<uint32_t>& rgba,
                        const render::GpuStats& stats, CaptureInfo& out) {
        const std::filesystem::path path =
            rex::filesystem::GetExecutableFolder() / "screenshots" / (file + ".gpu.png");
        if (!render::WritePng(path.string(), rgba, options.width, options.height)) {
            out.gpu_error = "couldn't write " + path.string();
            return;
        }
        out.gpu_path = path.string();
        // with `vr` on, the frame from each eye as the headset's are drawn
        // (native_view.cpp's VrFrame): xr::LayoutFor of this capture with the
        // last VR frame's views, or without a session a stand-in head at
        // LOCAL's origin facing -z, both eyes at it, 60 degrees each way. Into
        // <name>.gpu.L.png and <name>.gpu.R.png (black where the headset's
        // are, the fade left out; the dark around the window as the eyes
        // draw it, unless vr_window_quads; in songs with the tracks in
        // front); the
        // capture's own camera with the world's draws alone, at its size
        // (<name>.gpu.eyes.png: what the window in .gpu.L.png shows,
        // tools/vr_window_check.py); with a window, one eye as the
        // stand-in's but at the recenter's head facing the window, the
        // venue alone, without the tracks (<name>.gpu.C.png, the window
        // square on: the game camera); the layout in the reply's `vr` and
        // <name>.vr.json
        if (REXCVAR_GET(vr)) out.vr = VrPictures(frame, file, options, nullptr);
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
        options.view = render::RasterView::kFinal;
        if (options.filtering) {
            options.filtering = false;
            render::GpuStats nearest_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, options, rgba, nearest_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.nearest.png"))
                                     .string(),
                                 rgba, options.width, options.height);
            options.filtering = true;
        }
        // and with native_view_instancing on, each draw by itself,
        // <name>.gpu.uninstanced.png: the same frame, which should be the
        // same picture
        if (options.instancing) {
            options.instancing = false;
            render::GpuStats uninstanced_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, options, rgba, uninstanced_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.uninstanced.png"))
                                     .string(),
                                 rgba, options.width, options.height);
            options.instancing = true;
        }
        // and with native_view_hoist_passes on, the passes where the game
        // draws them, <name>.gpu.unhoisted.png: the same picture expected
        if (options.hoist_passes) {
            options.hoist_passes = false;
            render::GpuStats unhoisted_stats;
            if (render::GpuRenderer::Get().RenderFrame(frame, options, rgba, unhoisted_stats))
                render::WritePng((rex::filesystem::GetExecutableFolder() / "screenshots" /
                                  (file + ".gpu.unhoisted.png"))
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
            out.slot_waits = live.slot_waits;
            out.paint_lag_max = live.paint_lag_max;
            out.paints_lag3 = live.paints_lag3;
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
        out.xr_json = render::xr::Session::Get().MeasuredJson();
        return out;
    }

    // the VR frames' timing ring, written whole (a header alone without VR)
    std::string VrFrames(const std::string& name, VrFramesInfo& out) override {
        render::xr::Session& xs = render::xr::Session::Get();
        out.state = xs.GetStats().state;
        const std::string csv = xs.FramesCsv(out.frames);
        const std::filesystem::path dir = rex::filesystem::GetExecutableFolder() / "logs";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const std::filesystem::path path =
            dir / ("vr_frames-" + (name.empty() ? TimestampName() : name) + ".csv");
        std::ofstream file(path, std::ios::binary);
        file << csv;
        if (!file.good()) return "couldn't write " + path.string();
        out.path = path.string();
        return {};
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
        out.step_ms = std::move(paints.log.step_ms);
        out.latency_ms = std::move(paints.log.latency_ms);
        out.publish_latency_ms = std::move(paints.log.publish_latency_ms);
        const input::UiRoundTripStats trips = input::GetUiRoundTripStats(reset);
        out.ui_round_trip = {trips.runs, trips.p50, trips.p95, trips.max};
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

    std::string QueueMusicVideos(const std::vector<std::string>& shortnames, size_t& queued) override {
        queued = 0;
        const std::vector<std::filesystem::path> folders = video::MusicVideoFolders();
        if (folders.empty()) return "no music_videos_folder";
        const auto lower = [](std::string s) {
            for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
            return s;
        };
        // the packages' songs, then the game's own, as the Music Videos screen lists them
        std::vector<video::SongInfo> songs;
        for (const content::PackageSongs& p : content::PackageSongsNow(std::chrono::seconds(20)))
            for (const content::DtaSong& s : p.songs)
                songs.push_back({s.shortname, s.title, s.artist, s.song_length});
        for (const content::ListedSong& s : content::LastGameSongs())
            songs.push_back({s.shortname, s.title, s.artist, s.length_ms});
        std::vector<video::VideoJob> jobs;
        for (const std::string& name : shortnames) {
            const auto it = std::find_if(songs.begin(), songs.end(), [&](const video::SongInfo& s) {
                return lower(s.shortname) == lower(name);
            });
            if (it == songs.end()) return "no song " + name;
            video::VideoJob job;
            job.song = *it;
            job.folder = folders.front();
            jobs.push_back(std::move(job));
        }
        queued = video::QueueVideoJobs(std::move(jobs));
        return {};
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

    // As the window gets the mouse with focus: SDL's events, in its window
    // coordinates (the window's pixels over its pixel density), handed to the
    // window as its app context does. The pointer stays where it was moved.
    std::string MouseMove(int32_t x, int32_t y) override {
        mouse_x_ = x;
        mouse_y_ = y;
        const auto move = [&](rex::ui::WindowSDL& window, float scale) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_MOTION;
            event.motion.x = float(x) * scale;
            event.motion.y = float(y) * scale;
            window.HandleMouseEvent(event);
        };
        std::string error = MouseEvent(move);
        // The window keeps its pixel density to itself: the first move goes as
        // if it were 1, and where the pointer landed says what it is
        if (error.empty() && !mouse_scale_) {
            const input::HoverPointer landed = input::LastHoverPointer();
            mouse_scale_ = landed.moves && landed.x > 0 && x > 0 ? float(x) / float(landed.x) : 1.0f;
            if (std::abs(mouse_scale_ - 1.0f) > 0.01f) error = MouseEvent(move);
        }
        return error;
    }

    std::string MouseClick(bool right) override {
        return MouseEvent([&](rex::ui::WindowSDL& window, float scale) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.button = uint8_t(right ? SDL_BUTTON_RIGHT : SDL_BUTTON_LEFT);
            event.button.down = true;
            event.button.clicks = 1;
            event.button.x = float(mouse_x_) * scale;
            event.button.y = float(mouse_y_) * scale;
            window.HandleMouseEvent(event);
            event.type = SDL_EVENT_MOUSE_BUTTON_UP;
            event.button.down = false;
            window.HandleMouseEvent(event);
        });
    }

    std::string MouseWheel(int notches) override {
        return MouseEvent([&](rex::ui::WindowSDL& window, float scale) {
            SDL_Event event{};
            event.type = SDL_EVENT_MOUSE_WHEEL;
            event.wheel.y = float(notches);
            event.wheel.mouse_x = float(mouse_x_) * scale;
            event.wheel.mouse_y = float(mouse_y_) * scale;
            window.HandleMouseEvent(event);
        });
    }

    std::optional<MouseView> MouseRows() override {
        rex::ui::Window* window = window_;
        if (!window) return std::nullopt;
        uint32_t width = 0, height = 0;
        OnUIThread([&] {
            width = window->GetActualPhysicalWidth();
            height = window->GetActualPhysicalHeight();
        });
        const auto report = mouse_hover::RequestReport(width, height, std::chrono::seconds(2));
        if (!report) return std::nullopt;
        MouseView view;
        view.screen = report->screen;
        view.list = report->list;
        view.window_width = width;
        view.window_height = height;
        view.picture_x = report->picture.x;
        view.picture_y = report->picture.y;
        view.picture_w = report->picture.w;
        view.picture_h = report->picture.h;
        view.left = report->bounds.min.x;
        view.top = report->bounds.min.y;
        view.right = report->bounds.max.x;
        view.bottom = report->bounds.max.y;
        view.moves = report->moves;
        view.focus = report->focus;
        view.drawn = report->drawn;
        for (const auto& [what, box] : report->targets) {
            view.targets.push_back({what, box.min.x, box.min.y, box.max.x, box.max.y});
        }
        std::sort(view.targets.begin(), view.targets.end(), [](const auto& a, const auto& b) {
            return a.top != b.top ? a.top < b.top : a.left < b.left;
        });
        for (const auto& row : report->rows) {
            view.rows.push_back(MouseRow{row.display, row.showing, row.data, row.at.x, row.at.y,
                                         row.pickable, row.highlighted});
        }
        return view;
    }

    std::string SendMidi(std::span<const uint8_t> message) override {
        if (!input::InjectMidiKeysMessage(message)) {
            return "no MIDI keyboard driver runs: launch with --midi_keys=true (and "
                   "--midi_keys_test_device=true for a keyboard with no port)";
        }
        return {};
    }

    std::string LivelessInvite(const std::string& host, uint16_t port, bool force_flag) override {
        return online::FakeInvite(host, port, force_flag);
    }

    rooms::Status RoomsStatus() override { return rooms::GetStatus(); }

    std::string RoomsJoin(const std::string& code) override { return rooms::Join(code); }

    std::string RoomsConnect() override { return rooms::Connect(); }

    port_mapping::Status PortMappingStatus() override { return port_mapping::GetStatus(); }

    LightsView Lights() override {
        return {lights::Devices(), lights::TakeFakeCommands(), lights::PicoProblem()};
    }
    void LightsTest(lights::Command command, const std::optional<std::string>& key) override {
        lights::SendTest(command, key);
    }

    // as the window's close button: RequestClose skips the close request the
    // button makes (band3_app.h's OnWindowCloseRequested), so online play
    // lets go of the router's port mapping and the Rooms server here first,
    // and the Stage Kits go dark
    void Quit() override {
        rex::ui::Window* window = window_;
        app_context_->CallInUIThreadDeferred([window] {
            online::Stop();
            lights::Stop();
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

    // hands the window a mouse event, on the UI thread, as a focused window's
    // (MouseAsFocused); `scale` takes the window's pixels to SDL's window
    // coordinates (mouse_scale_)
    std::string MouseEvent(const std::function<void(rex::ui::WindowSDL&, float)>& send) {
        auto* window = static_cast<rex::ui::WindowSDL*>(window_);
        if (!window) return "no window";
        std::string error;
        OnUIThread([&] {
            const uint32_t physical = window->GetActualPhysicalWidth();
            if (!physical) {
                error = "the window is minimized (window offscreen restores it)";
                return;
            }
            input::MouseAsFocused(true);
            send(*window, mouse_scale_ ? mouse_scale_ : 1.0f);
            input::MouseAsFocused(false);
        });
        return error;
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
        render::xr::Session::Get().StartMeasuring();
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
                    {"geom_copy_bytes", p.geom_copy_bytes},
                    {"tex_copy_bytes", p.tex_copy_bytes},
                    {"tex_decode_bytes", p.tex_decode_bytes},
                    {"bones", p.bones},
                    {"deferred_decodes", p.deferred_decodes},
                    {"deferred_decode_us", p.deferred_decode_us},
                    {"deferred_decode_bytes", p.deferred_decode_bytes},
                    {"deferred_bc_blocks", p.deferred_bc_blocks},
                    {"deferred_bc_swizzled", p.deferred_bc_swizzled},
                    {"deferred_bc_rgba", p.deferred_bc_rgba},
                    {"deferred_r8", p.deferred_r8},
                    {"deferred_r8_rgba", p.deferred_r8_rgba}};
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
        out.parts_ms = {{"decode", g.decode_ms},
                        {"pre", g.pre_ms},
                        {"pre_plan", g.pre_plan_ms},
                        {"pre_walk", g.pre_walk_ms},
                        {"pre_targets", g.pre_targets_ms},
                        {"pre_arrays", g.pre_arrays_ms},
                        {"pre_upload", g.pre_upload_ms},
                        {"pre_record", g.pre_record_ms},
                        {"pre_acquire", g.pre_acquire_ms},
                        {"pre_first_draw", g.pre_first_draw_ms},
                        {"pre_draw_slow", g.pre_draw_slow_ms},
                        {"pre_uniform_slow", g.pre_uniform_slow_ms},
                        {"pre_submit", g.pre_submit_ms},
                        {"pre_wait", g.pre_wait_ms},
                        {"plan", g.plan_ms},
                        {"plan_setup", g.plan_setup_ms},
                        {"plan_walk", g.plan_walk_ms},
                        {"plan_targets", g.targets_ms},
                        {"premake", g.premake_ms},
                        {"plan_arrays", g.arrays_ms},
                        {"plan_arena", g.plan_arena_ms},
                        {"plan_reserve", g.plan_reserve_ms},
                        {"upload", g.upload_ms},
                        {"record", g.record_ms},
                        {"acquire", g.acquire_ms},
                        {"first_draw", g.first_draw_ms},
                        {"draw_slow", g.draw_slow_ms},
                        {"uniform_slow", g.uniform_slow_ms},
                        {"post_plan", g.post_plan_ms},
                        {"submit", g.submit_ms},
                        {"wait", g.wait_ms},
                        {"evict", g.evict_ms},
                        {"ahead", g.ahead_ms}};
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
                      {"instanced_draws", double(g.instanced_draws)},
                      {"instances_merged", double(g.instances_merged)},
                      {"world_draws", double(g.world_draws)},
                      {"ahead_gated", double(g.ahead_gated)},
                      {"ahead_used", double(g.ahead_used)},
                      {"ahead_fallback_passes", double(g.ahead_fallback_passes)},
                      {"passes", double(g.passes)},
                      {"passes_kept", double(g.passes_kept)},
                      {"passes_hoisted", double(g.passes_hoisted)},
                      {"rt_layers", double(g.rt_layers)},
                      {"rt_layers_mb", g.rt_layers_mb},
                      {"pre_passes", double(g.pre_passes)},
                      {"pre_submits", double(g.pre_submits)},
                      {"pre_targets_made", double(g.pre_targets_made)},
                      {"pre_arrays_grown", double(g.pre_arrays_grown)},
                      {"submits", double(g.submits)},
                      {"acquires", double(g.acquires)},
                      {"uniform_slow", double(g.uniform_slow)},
                      {"draw_slow", double(g.draw_slow)},
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
                      {"premade_used", double(g.premade_used)},
                      {"targets_premade", double(g.targets_premade)},
                      {"arrays_premade", double(g.arrays_premade)},
                      {"arrays_premade_used", double(g.arrays_premade_used)},
                      {"premade_unused", double(g.premade_unused)},
                      {"shared_depths", double(g.shared_depths)},
                      {"textures_first", double(g.textures_first)},
                      {"samples_hw", double(g.samples_hw)},
                      {"variant_draws", double(g.variant_draws)},
                      {"samples_npot", double(g.samples_npot)},
                      {"samples_small", double(g.samples_small)},
                      {"samples_aspect", double(g.samples_aspect)},
                      {"samples_mode", double(g.samples_mode)},
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
                              {"geom_copy_bytes", double(k.cost.geom_copy_bytes)},
                              {"tex_copy_bytes", double(k.cost.tex_copy_bytes)},
                              {"tex_decode_bytes", double(k.cost.tex_decode_bytes)},
                              {"game_ms", double(k.cost.game_ns) / 1e6}};
        out.gpu_timed = k.gpu_timed;
        for (int p = 0; p < render::gpu_timing::kParts; p++)
            out.gpu_ms.emplace_back(render::gpu_timing::PartName(uint8_t(p)), g.gpu_ms[p]);
        out.gpu_total_ms = std::move(k.gpu_total_ms);
        out.gpu_marks_dropped = g.gpu_marks_dropped;
        out.gpu_bad_spans = g.gpu_bad_spans;
        return out;
    }

    rex::Runtime* runtime_;
    rex::ui::WindowedAppContext* app_context_;
    rex::ui::Window* window_;
    // where `mouse move` last put the pointer, in the window's pixels, and
    // SDL's window coordinates per pixel, once the first move has shown it
    int32_t mouse_x_ = 0, mouse_y_ = 0;
    float mouse_scale_ = 0;
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
        // present_stats' ui_round_trip, while the harness can ask for it
        input::StartUiRoundTripProbe(app_context);
        stopping_ = false;
        thread_ = std::thread([this] { Run(); });
        REXLOG_INFO("Test server: listening on 127.0.0.1:{}", g_port);
    }

    void Stop() {
        if (!thread_.joinable()) return;
        input::StopUiRoundTripProbe();
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
