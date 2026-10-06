#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

#include "src/Render/gamma_ramp.h"
#include "src/Render/sync_gpu/sync_cp.h"

// N7 (out/research/n7_design.md): with renderer native
// (src/Render/renderer_mode.h), band3's graphics system in place of the SDK's
// emulated GPU, set as the runtime's (Band3App::OnPreSetup). The kernel
// reaches a graphics system only through IGraphicsSystem's virtuals
// (rex/system/interfaces/graphics.h), so this is all the game's GPU is:
//
// - Presentation: the SDK's own Direct3D 12 provider and presenter, made in
//   SetupPresentation before the window exists, so the SDK wires its overlays
//   and the window to them as it does the plugin's (rex_app.cpp:387-399) and
//   the native renderer draws on them as it always has (zero-copy included:
//   one device). Nothing ever refreshes the presenter's guest output; the
//   native renderer's drawer is the picture. On Linux the SDK's Vulkan
//   provider and presenter instead (untested); a build with neither keeps
//   the emulated GPU (CanPresentNativeOnly).
// - The guest's GPU: its MMIO window (0x7FC80000) to the sync-only command
//   processor (sync_cp.h), which a thread of its own ("band3 GPU sync") runs
//   whenever the guest moves the write pointer, acting on what the game waits
//   on and skipping draws; and a vertical blank thread ("band3 GPU vblank") at
//   the guest's refresh rate, or every millisecond while the frame cap paces
//   the game (SetVblankFreeRunning) or native_vblank_free_running says so.
//   Both are kernel threads (XHostThread):
//   the guest's interrupt handler, which they run (source 0 a vblank, 1 the
//   command processor's INTERRUPT packet), takes a spin lock and reads the
//   kernel's clock. The two threads may run it at once, as the plugin's
//   vsync and command processor threads may: the guest's spin lock is what
//   keeps them apart.
// - A watchdog on the vblank thread logs a wait the command processor has been
//   blocked in for 2 s (sync_monitor.h's StallWatch), and the log gets a
//   summary of the packets every 10 s, the waits by their wait interval
//   among them. native_query_log logs the occlusion queries' first ZPD
//   packets (SyncCommandProcessor::SetQueryLog).
//
// A lost device (a GPU hang) isn't recovered: it's logged and band3 aborts,
// so the crash trace has the device's DRED (crash_trace.h, Windows).

namespace band3::render::sync_gpu {

// what the sync-only GPU has done since it started, for the test harness
struct SyncGpuStats {
    SyncCpStats cp;
    uint64_t vblanks = 0;
    // the command processor thread's and the vblank thread's CPU time (kernel
    // and user), -1 where it can't be told (off Windows and Linux, or before
    // the thread started)
    double thread_ms = -1;
    double vblank_thread_ms = -1;
};

class Band3GraphicsSystem final : public rex::system::IGraphicsSystem {
public:
    Band3GraphicsSystem();
    ~Band3GraphicsSystem() override;
    Band3GraphicsSystem(const Band3GraphicsSystem&) = delete;
    Band3GraphicsSystem& operator=(const Band3GraphicsSystem&) = delete;

    rex::X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override;
    rex::X_STATUS SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                rex::system::KernelState* kernel_state) override;
    bool has_presentation() const override;
    rex::ui::GraphicsProvider* provider() const override;
    rex::ui::Presenter* presenter() const override;
    void SetInterruptCallback(uint32_t callback, uint32_t user_data) override;
    void InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) override;
    void EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) override;
    void Shutdown() override;

    // the vblank every millisecond rather than at the guest's refresh rate:
    // the frame cap's (frame_pacing.h's SetVsyncForCap), as the plugin's
    // vsync off did. native_vblank_free_running (Band3 → Debug) has it so
    // whatever this is told, for a run nothing paces (frame_cap off,
    // rnd_sync 0). Any thread, before or after the threads start.
    void SetVblankFreeRunning(bool free_running);
    // the display gamma ramp the guest's DC_LUT registers set
    GammaRamp DisplayGamma() const;
    SyncGpuStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// whether this build has something to present with but the emulated GPU:
// the SDK's Direct3D 12 provider (Windows) or its Vulkan one (Linux), by the
// runtime's REX_HAS_D3D12 / REX_HAS_VULKAN. PlanStartupGpu's `presentable`.
bool CanPresentNativeOnly();

// The sync-only GPU while it's the runtime's, from its construction in
// Band3App::OnPreSetup to its destruction; null with the emulated GPU (every
// run with renderer emulated or both). Any thread.
Band3GraphicsSystem* Active();

}  // namespace band3::render::sync_gpu
