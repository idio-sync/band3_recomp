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
    // the frame with them; with null eyes, ends it without a layer.
    bool Present(const FrameViews& f, void* const eye[2], double begin_to_present_ms);
    void AddRenderMs(int eye, double ms, double gpu_ms, double repose_ms);
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
