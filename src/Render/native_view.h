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

// Experimental: the native view (bind_native_view, F9) draws what RB3
// sent to the back buffer last frame on the CPU, from guest memory alone,
// beside the emulated GPU's picture.
//
// The native renderer (renderer = native, bind_renderer F8) draws the same
// pictures on the game's window in place of the emulated GPU's, under the
// SDK's ImGui overlays, through a UI drawer on the SDK's presenter (z 0, ImGui
// is 64). On Windows it samples gpu_view's output textures where they are
// (zero-copy, see gpu_view.h); elsewhere, or when that can't be done, it
// uploads each frame through the SDK's immediate drawer. The emulated GPU
// still runs and its picture is still painted underneath, then covered;
// with emulated_gpu_while_native skip_draws (the default) or swap_only it
// skips the game's draws meanwhile (gpu_skip.h), so after F8 back to
// emulated the native renderer draws the window on until the emulated GPU
// has drawn whole frames again.
// Its worker sleeps until the game publishes a capture (scene_capture.h's
// WaitForCapture) and draws the newest, published to the window a steady
// delay after the game presented it (present_model.h's PublishPacer); each
// paint shows the newest published. With native_present_pipeline (off until
// checked in game), on the zero-copy path it submits a frame without waiting
// for the GPU and waits for it only to publish it, so it records the next
// capture while the GPU draws the one before (one frame in flight besides the
// one recorded, never more); off, it waits for each once submitted. A frame
// the GPU hasn't finished in 2 s holds it, the window keeping the last frame,
// until the GPU does. Each time it turns native, the window
// shows only frames from captures published since (PresentSlots).
// Once it has been native, capture records the passes RB3 draws into
// textures all the time for the rest of the session, as
// native_view_record_targets does (scene_capture.cpp's TrackSettings): set it
// at launch, as outfits are composed only once.
//
// BAND3_NATIVE_VIEW_DUMP=<path> starts capturing at launch without the window
// and saves a frame every five seconds as <path>.NNN.cap (and .cap.txt with its
// numbers), for tools/native_view_replay to draw.

namespace band3::render {

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

// The live view, for the test harness (`native_view on`): the window's
// renderer, the same worker drawing each new capture on the same backend,
// without the window. The window and the live view share it, and the last to
// set a size wins, unless the native renderer is drawing the window: its
// size, the window's picture's, wins over both.
struct LiveViewStats {
    bool gpu = false;  // what drew the last frame, or would draw the next
    uint32_t width = 0, height = 0;
    bool post = true;  // with RB3's post-processing (RasterOptions::post)
    uint64_t captured = 0;      // frames captured since it started
    uint64_t rendered = 0;      // of those, the ones drawn
    uint64_t skipped_busy = 0;  // and the ones it was still drawing another for
    // of those drawn, the ones with no world: they drew none, under even/odd
    // rendering, and weren't composed with the world frame before them
    uint64_t worldless = 0;
    // each one's time, GpuStats::ms or RasterStats::ms: on the zero-copy
    // path the worker's recording and submitting it, the GPU's time after
    // left out
    std::vector<double> ms;
    // GpuStats::wait_ms, the GPU's frames only: on the zero-copy path the
    // time the worker waited for the GPU to finish the frame before
    // publishing it, its time that recording the next frame didn't hide
    std::vector<double> wait_ms;
    // the most of the worker's frames the GPU had at once (submitted and
    // not finished): 1 unless native_present_pipeline is on, then 2 while it
    // records a frame as the GPU draws the one before, never more
    uint32_t in_flight_max = 0;
    // with emulated_gpu off: whether the window is minimized now, so the
    // worker draws nothing (NativePresentMinimized), and since the numbers
    // started the milliseconds it was, and the captures the game published
    // meanwhile, which it left undrawn (neither rendered nor skipped_busy)
    bool paused = false;
    double paused_ms = 0;
    uint64_t paused_captures = 0;
    // The frames drawn by kind (frame_compose.h's FrameKind, by_kind's
    // index): how many, the captures skipped while one of them was being
    // drawn (so the frames that were too slow are the kind charged), each
    // one's ms and wait_ms as above, and totals over them of the worker's
    // parts and what it sent, made and let go of (GpuStats: the GPU's
    // frames), the frames among them that showed the kept post buffer, that
    // rebuilt the arena and that were composed, and what capturing them cost
    // the game's thread (FrameCapture::Cost)
    struct Kind {
        uint64_t rendered = 0;
        uint64_t skipped_busy = 0;
        std::vector<double> ms, wait_ms;
        GpuStats gpu;
        uint64_t gpu_frames = 0, shows_kept = 0, arena_rebuilt = 0, composed = 0;
        FrameCapture::Cost cost;
        // the most the GPU kept after one of them (GpuStats::resident_meshes
        // and the rest)
        uint32_t peak_meshes = 0, peak_textures = 0, peak_rts = 0;
        double peak_texture_array_mb = 0, peak_arena_mb = 0;
    };
    Kind by_kind[kFrameKinds];
};
// starts it, or starts its numbers over at a new size if it's on; on the UI
// thread, as the GPU device starts there. `post` off leaves RB3's
// post-processing out, to measure what it costs.
void StartLiveView(uint32_t width, uint32_t height, bool post = true);
// stops it, and capturing with it unless the window still wants it
void StopLiveView();
bool LiveViewOn();
LiveViewStats GetLiveViewStats();

// The native renderer on the window. Start on the UI thread once the runtime
// has its graphics system (OnPostSetup; the presenter doesn't exist at
// OnCreateDialogs): it adds the drawer, which draws nothing while renderer is
// emulated, and follows the setting from then on, starting the worker when
// it turns native and letting it go when it turns emulated. `immediate_drawer`
// is the SDK's, for the upload path. Stop it at shutdown before
// GpuRenderer::Shutdown: it takes the drawer off and waits for the GPU to
// finish with the outputs its paints sampled.
void StartNativePresent(rex::ui::Presenter* presenter, rex::ui::GraphicsProvider* provider,
                        rex::ui::Window* window,
                        std::function<rex::ui::ImmediateDrawer*()> immediate_drawer);
void StopNativePresent();
// The SDK's window was minimized or restored (Band3App's OnWindowMinimized
// and OnWindowRestored), on the UI thread. With emulated_gpu off the worker
// draws nothing while it's minimized (a screenshot asked for aside), and once
// it's restored the window shows the last frame published until the first
// from a capture published since (a whole picture, a frame or two later);
// each frame published asks the
// window to paint, so a restored window is asked at once, and where the
// system can't be asked whether the window can be seen (not Windows),
// minimized holds the paints back until it's restored. With the emulated GPU
// on, nothing changes.
void NativePresentMinimized(bool minimized);
// whether the native renderer is drawing the window (renderer = native, started)
bool NativePresenting();
// The window's paints since their numbers last started over (`since`),
// whichever renderer drew them, for the harness's present_stats; `reset`
// starts them over once read. `path`: how the native renderer's frames reach
// the window, "zero-copy" or "upload", or empty while it isn't drawing it.
// A paint's time is when the drawer is asked to draw, on the UI thread, not
// when the picture reaches the screen. Any thread.
struct PresentPaintStats {
    PaintLog log;
    std::chrono::steady_clock::time_point since;
    std::string path;
};
PresentPaintStats GetPresentPaintStats(bool reset);
// the size it draws the window's picture at (the picture's, or less by
// native_max_height), false while it isn't drawing it
bool NativePresentDrawSize(uint32_t& width, uint32_t& height);
// o's target_scale and shadow_scale as the native renderer draws a picture
// o.height tall with (native_view_target_scale, native_view_shadow_scale)
void ScaleForPicture(RasterOptions& o);
// For the harness's screenshot: the picture the native renderer last drew for
// the window, at its size (the newest frame the window showed, or the newest
// drawn if no paint has shown one, minimized), waiting up to `wait` for the
// first; an error, or empty. Any thread.
std::string NativePresentedPicture(std::vector<uint32_t>& rgba, uint32_t& width,
                                   uint32_t& height, std::chrono::milliseconds wait);

// reads BAND3_NATIVE_VIEW_DUMP
void StartDumpIfRequested();
// at shutdown: stops the dump and the live view
void StopNativeView();

}  // namespace band3::render
