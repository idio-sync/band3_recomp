#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

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
// still runs and its picture is still painted underneath, then covered.
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
    std::vector<double> ms;     // each one's time, GpuStats::ms or RasterStats::ms
    std::vector<double> wait_ms;  // GpuStats::wait_ms, the GPU's frames only
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
// whether the native renderer is drawing the window (renderer = native, started)
bool NativePresenting();
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
