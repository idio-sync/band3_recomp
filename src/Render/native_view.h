#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "src/Render/frame_compose.h"
#include "src/Render/gpu_view.h"
#include "src/Render/present_model.h"
#include "src/Render/soft_raster.h"

namespace rex::ui {
class GraphicsProvider;
class Presenter;
class Window;
}  // namespace rex::ui

// The native view (bind_native_view, F9) draws what RB3 sent to the back
// buffer last frame from guest memory alone, beside the emulated GPU's picture.
//
// The native renderer draws the same pictures on the game's window in place
// of the emulated GPU's, under the ImGui overlays, through a UI drawer on the
// presenter (z 0, ImGui is 64). On Windows it samples gpu_view's outputs
// zero-copy; otherwise it uploads each frame through the immediate drawer.
// The emulated GPU still runs underneath; with emulated_gpu_while_native
// skip_draws or swap_only it skips the game's draws (gpu_skip.h), so after F8
// back to emulated the native renderer keeps drawing until the emulated GPU
// has drawn whole frames again.
// The worker waits for a capture (WaitForCapture), draws the newest and
// publishes it a steady delay after the game presented it (PublishPacer).
// With native_present_pipeline, on the zero-copy path it records the next
// capture while the GPU draws the one before (at most one in flight besides
// the one recorded). A frame the GPU hasn't finished in 2 s holds it.
// Each turn to native shows only captures published since (PresentSlots).
// Once it has been native, capture records texture passes for the rest of the
// session (TrackSettings), since outfits are composed only once.
//
// BAND3_NATIVE_VIEW_DUMP=<path> captures from launch without the window and
// saves a frame every 5 s as <path>.NNN.cap (+ .cap.txt) for
// tools/native_view_replay.

namespace band3::render {

// Whether RB3's Game exists (set from rb3e_events.cpp's Game____ct and
// Game____dt). Any thread. Meshes and textures are kept by the clock only
// then (RasterOptions::clock_keep).
void SetInSong(bool in_song);
bool InSong();

class NativeViewDialog : public rex::ui::ImGuiDialog {
 public:
    using DrawerGetter = std::function<rex::ui::ImmediateDrawer*()>;
    NativeViewDialog(rex::ui::ImGuiDrawer* imgui_drawer, DrawerGetter drawer);
    ~NativeViewDialog() override;

    void Toggle();

 protected:
    void OnDraw(ImGuiIO& io) override;

 private:
    // native_view_backend is gpu and the device starts (on the first call)
    static bool WantsGpu();

    DrawerGetter drawer_;
    bool visible_ = false;
    RasterOptions options_;
    std::unique_ptr<rex::ui::ImmediateTexture> texture_;
    uint64_t texture_frame_ = 0;
};

// The live view, for the test harness (`native_view on`): the window's worker
// and backend without the window. The last to set a size wins, unless the
// native renderer is drawing the window: then the window picture's size wins.
struct LiveViewStats {
    bool gpu = false;  // what drew the last frame, or would draw the next
    uint32_t width = 0, height = 0;
    bool post = true;  // RasterOptions::post
    uint64_t captured = 0;      // since it started
    uint64_t rendered = 0;      // of those, the ones drawn
    uint64_t skipped_busy = 0;  // and the ones it was still drawing another for
    // drawn with no world, under even/odd rendering, and not composed with
    // the world frame before them
    uint64_t worldless = 0;
    // GpuStats::ms or RasterStats::ms; on the zero-copy path the GPU's time
    // is left out
    std::vector<double> ms;
    // GpuStats::wait_ms, GPU frames only: the wait for the GPU before
    // publishing that recording the next frame didn't hide
    std::vector<double> wait_ms;
    // most worker frames submitted and unfinished at once: 1, or 2 with
    // native_present_pipeline
    uint32_t in_flight_max = 0;
    // renderer native: minimized now (worker draws nothing), total ms
    // minimized, and the captures left undrawn meanwhile (neither rendered
    // nor skipped_busy)
    bool paused = false;
    double paused_ms = 0;
    uint64_t paused_captures = 0;
    // camera cuts in frames drawn (CameraCuts), vel_frame resets, and cuts a
    // reset confirmed; unlike the slow-frame log, no frames are left out
    uint64_t camera_cuts = 0, vel_resets = 0, cuts_confirmed = 0;
    // Frames drawn, indexed by FrameKind. skipped_busy is charged to the kind
    // being drawn; gpu holds totals over the GPU's frames.
    struct Kind {
        uint64_t rendered = 0;
        uint64_t skipped_busy = 0;
        std::vector<double> ms, wait_ms;
        GpuStats gpu;
        uint64_t gpu_frames = 0, shows_kept = 0, arena_rebuilt = 0, composed = 0;
        FrameCapture::Cost cost;
        // per-frame maxima of plan_ms and its parts (the other fields 0);
        // plans over kPlanSpikeMs, and of those the ones within
        // kSpikeAfterCut game frames of a camera cut
        GpuStats plan_most;
        uint64_t plan_spikes = 0, plan_spikes_at_cut = 0;
        // the most the GPU kept resident after one of them
        uint32_t peak_meshes = 0, peak_textures = 0, peak_rts = 0;
        uint32_t peak_meshes_by_time = 0, peak_textures_by_time = 0;
        double peak_texture_array_mb = 0, peak_arena_mb = 0, peak_rts_mb = 0;
        // native_gpu_timestamps: GPU frames with GPU timings (gpu.gpu_ms
        // totals their parts) and each one's gpu_total_ms
        uint64_t gpu_timed = 0;
        std::vector<double> gpu_total_ms;
    };
    Kind by_kind[kFrameKinds];
    static constexpr double kPlanSpikeMs = 8;
    static constexpr int64_t kSpikeAfterCut = 2;
};
// starts it, or restarts its numbers at a new size; UI thread (the GPU device
// starts there). `post` off leaves RB3's post-processing out.
void StartLiveView(uint32_t width, uint32_t height, bool post = true);
// stops it, and capturing with it unless the window still wants it
void StopLiveView();
bool LiveViewOn();
LiveViewStats GetLiveViewStats();

// The native renderer on the window. Start on the UI thread at OnPostSetup
// (no presenter at OnCreateDialogs); it then follows the renderer setting,
// starting and releasing the worker. `immediate_drawer` is for the upload
// path. Stop before GpuRenderer::Shutdown: it waits for the GPU to finish with
// the outputs its paints sampled.
void StartNativePresent(rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
                        rex::ui::Window* window,
                        std::function<rex::ui::ImmediateDrawer*()> immediate_drawer);
void StopNativePresent();
// From Band3App's OnWindowMinimized/OnWindowRestored, UI thread. With renderer
// native the worker draws nothing while minimized (except for a screenshot);
// once restored the window shows the last frame until one from a newer
// capture. Off Windows, where visibility can't be queried, minimized also
// holds paints back.
void NativePresentMinimized(bool minimized);
// whether the native renderer is drawing the window
bool NativePresenting();
// The window's paints since `since`, either renderer, for the harness's
// present_stats; `reset` restarts them. `path`: "zero-copy", "upload", or
// empty while the native renderer isn't drawing. Paint times are when the
// drawer is asked to draw, not when the screen shows it. Any thread.
struct PresentPaintStats {
    PaintLog log;
    std::chrono::steady_clock::time_point since;
    std::string path;
};
PresentPaintStats GetPresentPaintStats(bool reset);
// recent mean ms from the game's Present to the first paint showing the frame
// (PaintRecorder::RecentLatencyMs); 0 while not drawing the window. Any thread.
double RecentPaintLatencyMs();
// capped by native_max_height; false while not drawing the window
bool NativePresentDrawSize(uint32_t& width, uint32_t& height);
// sets o's target_scale and shadow_scale for a picture o.height tall
// (native_view_target_scale, native_view_shadow_scale)
void ScaleForPicture(RasterOptions& o);
// For the harness's screenshot: the newest frame the window showed (or the
// newest drawn, if minimized), waiting up to `wait` for the first. Returns an
// error, or empty. Any thread.
std::string NativePresentedPicture(std::vector<uint32_t>& rgba, uint32_t& width,
                                   uint32_t& height, std::chrono::milliseconds wait);

// reads BAND3_NATIVE_VIEW_DUMP
void StartDumpIfRequested();
// at shutdown: stops the dump and the live view
void StopNativeView();

}  // namespace band3::render
