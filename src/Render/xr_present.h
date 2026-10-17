#pragma once

// Experimental VR output (the `vr` setting): the native renderer's eyes
// shown in a headset through OpenXR, on Windows with Direct3D 12 only;
// elsewhere every call is a no-op and `vr` does nothing.
//
// The XrSession is bound (XR_KHR_D3D12_enable) to the SDK's own device and
// direct queue, which gpu_view's outputs already live on (SetDevice). Each
// eye is a swapchain of the runtime's recommended size, drawn by present.hlsl
// from that eye's output, on band3's own command list on the SDK's queue,
// as native_view.cpp's D3D12Present draws the window.
//
// The OpenXR loader (Apache-2.0, the runtime's own: SteamVR ships one) is
// loaded at run time, never linked: openxr_loader.dll from the usual search
// path, else SteamVR's. Nothing of OpenXR is loaded while `vr` is off, and
// band3 goes on without VR (logged once) if the loader, the runtime or the
// session can't be had. Making the instance starts the runtime (SteamVR's
// windows appear), so that is done on a thread of its own.
//
// native_view.cpp's worker drives the frames (Session::Update, then
// WaitFrame / Present per frame) once the session runs; the game thread is
// untouched.
//
// The HUD panel (vr_hud; vr_pose.h's HudBoundary) is a third swapchain, the
// eyes' format and size, shown as a quad layer (XrCompositionLayerQuad) in
// LOCAL space over the eyes' projection layer, blended by its alpha
// (premultiplied: present.hlsl's PSPresentHud; into images the runtime reads
// as sRGB, PSPresentHudLinear through an sRGB view, so partly covered pixels
// stay premultiplied in linear light). It hangs where
// vr::HudPlacement puts it from a head pose taken at the first frame the
// views were located, again at each recenter (vr_hud_recenter, or the
// runtime moving LOCAL space). Its image is drawn only when the worker has
// a new panel picture; frames in between show the image last released, as
// a quad layer may.

#include <cstdint>
#include <string>

#include "src/Render/vr_pose.h"

namespace band3::render::xr {

// UI thread, as the native presenter starts (Windows): the SDK's
// ID3D12Device and ID3D12CommandQueue
void SetDevice(void* d3d12_device, void* d3d12_queue);

// one frame's timing and views (LOCAL space), from xrWaitFrame and
// xrLocateViews
struct FrameViews {
    int64_t display_time = 0;    // XrTime, ns
    int64_t display_period = 0;  // ns
    bool render = false;         // shouldRender
    // the views' poses are the headset's (or vr_fake_pose's); else a
    // stand-in head at the origin
    bool located = false;
    vr::XrPose pose[2];
    vr::Fov fov[2];
};

// for native_view stats' `xr` block
struct Stats {
    std::string state = "off";  // the session's state, or why there's none
    std::string runtime, system;
    uint32_t width = 0, height = 0;  // each eye's swapchain
    int64_t display_period_ns = 0;
    uint64_t frames = 0;   // ended with the eyes' layer
    uint64_t skipped = 0;  // ended without (no swapchain image)
    // of all ended, those the runtime said it wouldn't show (shouldRender
    // off: the session isn't visible, as with SteamVR's idle null headset)
    uint64_t hidden = 0;
    // frames whose drawing (xrBeginFrame to xrEndFrame) took longer than
    // the display period
    uint64_t late = 0;
    double wait_ms = 0;     // in xrWaitFrame, summed
    // each eye's drawing: the CPU's up to submitting (GpuStats::ms), the GPU's
    // (native_gpu_timestamps' gpu_total_ms), and RePose's copy, summed
    double render_ms[2] = {0, 0}, gpu_ms[2] = {0, 0}, repose_ms[2] = {0, 0};
    double present_ms = 0;  // acquiring, drawing into and releasing the images, summed
    double frame_ms = 0;    // xrBeginFrame to xrEndFrame, summed
    bool srgb_view = false;  // the images are typed sRGB: present.hlsl's linear variant
    // the HUD panel's images are drawn through an sRGB view (typed sRGB, or
    // typeless in an sRGB swapchain): PSPresentHudLinear
    bool hud_srgb_view = false;
    // the HUD panel: pictures drawn (AddHudMs) and their drawing as the
    // eyes' (CPU to submitting, GPU), summed; pictures copied into its
    // image; frames ended with its quad layer; recenters
    uint64_t hud_renders = 0, hud_copies = 0, hud_layers = 0, recenters = 0;
    double hud_render_ms = 0, hud_gpu_ms = 0;
};

class Session {
 public:
    static Session& Get();

    // Worker, each loop: with `want` (the `vr` setting), starts making the
    // instance and session (once), polls the runtime's events and begins
    // or ends the session as they say. Whether frames are to be run.
    bool Update(bool want);
    // each eye's swapchain size
    uint32_t Width() const;
    uint32_t Height() const;
    // xrWaitFrame (the runtime's pacing), xrBeginFrame, xrLocateViews. With
    // vr_fake_pose the head turns slowly instead (the null driver has no
    // tracking). False if the frame couldn't begin.
    bool WaitFrame(FrameViews& f);
    // Draws each eye's output (an ID3D12Resource gpu_view's outputs keep in
    // ALL_SHADER_RESOURCE, finished on the GPU) into its swapchain and ends
    // the frame with them; with null eyes, ends it without a layer. `hud`, a
    // new HUD panel picture (an output likewise), goes into the panel's
    // image; null keeps the image last drawn. With vr_hud the frame ends
    // with the panel's quad layer over the eyes once it has an image.
    bool Present(const FrameViews& f, void* const eye[2], void* hud, double begin_to_present_ms);
    // whether the HUD panel's output may be drawn again: the SDK's queue has
    // finished copying the last picture out of it (nothing else orders it
    // after SDL's queue). Never waits.
    bool HudFree();
    void AddRenderMs(int eye, double ms, double gpu_ms, double repose_ms);
    void AddHudMs(double ms, double gpu_ms);
    Stats GetStats();
    // native_view stats' `xr` block, counted from the last StartMeasuring
    // (the harness's `native_view on`); "" while `vr` was never on
    void StartMeasuring();
    std::string MeasuredJson();
    // the anchor and eyes of the last frame presented, for `capture`'s
    // .gpu.L.png and .gpu.R.png; false before one
    bool LastEyes(vr::Eye eyes[2], uint32_t& width, uint32_t& height);
    void NoteEyes(const vr::Eye eyes[2]);

 private:
    Session() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

}  // namespace band3::render::xr
