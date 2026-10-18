#pragma once

// Experimental VR output (the `vr` setting): the native renderer's eyes
// shown in a headset through OpenXR, on Windows with Direct3D 12 only;
// elsewhere every call is a no-op and `vr` does nothing.
//
// The XrSession is bound (XR_KHR_D3D12_enable) to the SDK's own device and
// direct queue, which gpu_view's outputs already live on (SetDevice). Each
// eye is a swapchain of the runtime's recommended size, drawn by present.hlsl
// from that eye's output, on band3's own command list on the SDK's queue,
// as native_view.cpp's D3D12Present draws the window. That queue waits on
// the GPU for SDL_gpu's fences before copying (Present's waits,
// vr_gpu_queue_wait), so a frame goes to the runtime as soon as its drawing
// is submitted; the worker checks the copies finished after xrEndFrame.
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
//
// What the eyes show and where the panel goes is VrFrame's choice each frame
// (LayoutFor: vr_pose.h's ResolveFrame with the vr_* settings), handed to
// Present: the eyes through the game camera's window, with the dark around
// it as nine more quad layers between the eyes' and the panel's (one tiny
// swapchain cleared black at vr_window_dim's alpha, cleared again as that
// changes), and the panel in the window or on its own place; or black eyes
// and the panel alone. The eyes fade in from black after a cut (PresentConstants'
// frame_size.w); the panel doesn't.
//
// Each runtime frame's timing (vr_timing.h's FrameTiming) is filled by
// WaitFrame, the worker's VrFrame and Present, and handed to Record, which
// keeps it in a ring for the harness's `vr_frames` (FramesCsv), counts it in
// the stats and logs a slow one (at most one a second).

#include <cstdint>
#include <string>

#include "src/Render/vr_pose.h"
#include "src/Render/vr_timing.h"

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
    // the head at the last recenter, which the panel and the window hang
    // from; until the views were first located, between this frame's eyes
    bool have_head = false;
    vr::XrPose head;
    // where the frame's time goes: WaitFrame starts it, the worker and
    // Present fill their parts, Record keeps it
    vr::FrameTiming timing;
};

// for native_view stats' `xr` block
struct Stats {
    std::string state = "off";  // the session's state, or why there's none
    std::string runtime, system;
    uint32_t width = 0, height = 0;  // each eye's swapchain
    // the headset's refresh (vr_timing.h's RefreshPeriod), and the period
    // the runtime last predicted, twice it or more while it throttles band3
    int64_t display_period_ns = 0, predicted_period_ns = 0;
    uint64_t frames = 0;   // ended with the eyes' layer
    uint64_t skipped = 0;  // ended without (no swapchain image)
    // of all ended, those the runtime said it wouldn't show (shouldRender
    // off: the session isn't visible, as with SteamVR's idle null headset)
    uint64_t hidden = 0;
    // frames whose drawing took longer than the display period: from
    // WaitFrame's return (xrBeginFrame and xrLocateViews done) to xrEndFrame's
    // return, the CPU's part of the frame without xrWaitFrame's wait. Whether
    // the runtime then showed it late is `missed`.
    uint64_t late = 0;
    // Record's (vr_timing.h's Classify): frames recorded (the last one's
    // index); the runtime's display time moving on 1.5 periods or more, and
    // the refreshes so missed; xrWaitFrame's return to xrEndFrame's over the
    // period; xrWaitFrame returning 1.5 periods or more after the frame
    // before's; eyes drawn from the capture the frame before drew; eyes
    // drawn from a capture with newer ones published by Present; and logged
    // as slow (or would have been but for the rate limit)
    uint64_t recorded = 0, missed = 0, slots_missed = 0, over_period = 0, late_wakes = 0;
    uint64_t repeats = 0, behind = 0, slow = 0;
    // frames the runtime predicted a throttled period for (vr_timing.h's
    // FrameTiming::throttled), and those whose copies the SDK's queue waited
    // on the GPU for (Present's waits) rather than the CPU
    uint64_t throttled = 0, queue_waits = 0;
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
    // frames ended with the eyes through the game camera's window, with
    // black eyes (no world to see), and the cuts faded; with the tracks in
    // the eyes (vr_track_3d)
    uint64_t window_frames = 0, black_frames = 0, fades = 0, track_frames = 0;
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
    // ALL_SHADER_RESOURCE) into its swapchain and ends the frame with them (a
    // null one dark red or blue). `hud`, a new HUD panel picture (an output
    // likewise), goes into the panel's image; null keeps the image last
    // drawn. `layout` says the rest: black eyes, their fade, the dark around
    // the window, and the panel's quad layer over them all (once it has an
    // image). The outputs are finished on the GPU, or `waits` (up to
    // kQueueWaits, null fences skipped) are the fences SDL_gpu signals as
    // they finish (GpuRenderer::OutputFence), which the SDK's queue waits on
    // before its copies: the images are released and xrEndFrame called
    // without the CPU waiting for the GPU (OpenXR's D3D12 binding asks only
    // that their writes be submitted to the session's queue first). Fills
    // f.timing's Present part, its queue_wait whether `waits` were queued.
    static constexpr int kQueueWaits = 3;
    struct QueueWait {
        void* fence = nullptr;  // ID3D12Fence on the SDK's device
        uint64_t value = 0;
    };
    bool Present(FrameViews& f, void* const eye[2], void* hud, const vr::FrameLayout& layout,
                 double begin_to_present_ms, const QueueWait* waits = nullptr);
    // whether the SDK's queue has finished the last Present's copies (and
    // with them the waits queued before them). Never waits.
    bool CopiesDone();
    // Worker, once a frame's done (after the window's picture): classifies
    // `t` against the frame before, counts it, keeps it in the ring and logs
    // it if slow
    void Record(vr::FrameTiming& t);
    // the ring's frames (the last vr::kRingFrames), oldest first, as
    // vr_timing.h's CSV; `frames` says how many
    std::string FramesCsv(uint64_t& frames);
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
    // the views of the last frame presented and its fade, for `capture`'s
    // .gpu.L.png and .gpu.R.png (LayoutFor of the captured frame); false
    // before one
    bool LastFrame(FrameViews& f, float& fade, uint32_t& width, uint32_t& height);
    void NoteFrame(const FrameViews& f, float fade);

 private:
    Session() = default;
    struct Impl;
    Impl* impl_ = nullptr;
};

// The frame's layout for `fc` seen with `f`'s views: vr_pose.h's ResolveFrame
// with the settings (vr_viewpoint, vr_hud*, vr_window_width, vr_window_dim, vr_world_scale,
// vr_anchor, vr_track_*) and `in_song` (native_view.h: InSong and SongScreen), its fade 0. A
// vr_viewpoint not made yet is logged once and seen from vr_anchor. In songs
// with vr_track_3d, the capture's track cameras (TrackCamerasOf), their poses
// held by `hold` if given (a camera that moves anyway is logged, at most once
// a second). Any thread; every platform.
vr::FrameLayout LayoutFor(const FrameCapture& fc, const FrameViews& f, bool in_song,
                          vr::TrackHold* hold = nullptr);

}  // namespace band3::render::xr
