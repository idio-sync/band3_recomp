#pragma once

#include <functional>
#include <memory>

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

// reads BAND3_NATIVE_VIEW_DUMP
void StartDumpIfRequested();
void StopNativeView();

}  // namespace band3::render
