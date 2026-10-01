#pragma once

#include <functional>
#include <memory>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/immediate_drawer.h>

#include "src/Render/soft_raster.h"

// Experimental: the native view (bind_native_view, F7) draws what RB3
// sent to the back buffer last frame on the CPU, from guest memory alone,
// beside the emulated GPU's picture.
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
// set a size wins.
struct LiveViewStats {
    bool gpu = false;  // what drew the last frame, or would draw the next
    uint32_t width = 0, height = 0;
    uint64_t captured = 0;      // frames captured since it started
    uint64_t rendered = 0;      // of those, the ones drawn
    uint64_t skipped_busy = 0;  // and the ones it was still drawing another for
    std::vector<double> ms;     // each one's time, GpuStats::ms or RasterStats::ms
    std::vector<double> wait_ms;  // GpuStats::wait_ms, the GPU's frames only
};
// starts it, or starts its numbers over at a new size if it's on; on the UI
// thread, as the GPU device starts there
void StartLiveView(uint32_t width, uint32_t height);
// stops it, and capturing with it unless the window still wants it
void StopLiveView();
bool LiveViewOn();
LiveViewStats GetLiveViewStats();

// reads BAND3_NATIVE_VIEW_DUMP
void StartDumpIfRequested();
// at shutdown: stops the dump and the live view
void StopNativeView();

}  // namespace band3::render
