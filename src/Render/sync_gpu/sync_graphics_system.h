#pragma once

#include <cstdint>
#include <memory>

#include <rex/system/interfaces/graphics.h>

#include "src/Render/gamma_ramp.h"
#include "src/Render/sync_gpu/sync_cp.h"

// The runtime's graphics system with renderer native, replacing the SDK's
// emulated GPU (set in Band3App::OnPreSetup). The kernel only uses
// IGraphicsSystem's virtuals, so this is the game's entire GPU:
//
// - Presentation: the SDK's own D3D12 provider and presenter (Vulkan on
//   Linux, untested), made in SetupPresentation before the window exists so
//   the SDK wires overlays and the window to them as for the plugin
//   (rex_app.cpp:387-399). The guest output is never refreshed; the native
//   renderer's drawer is the picture.
// - The MMIO window (0x7FC80000) goes to the sync-only CP (sync_cp.h), run by
//   "band3 GPU sync" on each write-pointer move; "band3 GPU vblank" ticks at
//   the guest refresh rate or every 1 ms (SetVblankFreeRunning). Both are
//   XHostThreads because the guest's interrupt handler they run (source 0
//   vblank, 1 INTERRUPT) takes a spin lock and reads the kernel clock; that
//   spin lock keeps the two apart.
// - The vblank thread also runs the StallWatch and logs a packet summary
//   every 10 s.
//
// A lost device isn't recovered: it's logged and band3 aborts so the crash
// trace has DRED (crash_trace.h, Windows).

namespace band3::render::sync_gpu {

struct SyncGpuStats {
    SyncCpStats cp;
    uint64_t vblanks = 0;
    // kernel + user CPU time; -1 if unknown (other OSes, or not started)
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

    // vblank every 1 ms instead of at the refresh rate, set by the frame cap
    // (frame_pacing.h's SetVsyncForCap). native_vblank_free_running forces
    // it on regardless. Any thread, any time.
    void SetVblankFreeRunning(bool free_running);
    GammaRamp DisplayGamma() const;
    SyncGpuStats Stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// REX_HAS_D3D12 or REX_HAS_VULKAN; PlanStartupGpu's `presentable`
bool CanPresentNativeOnly();

// null unless renderer native. Any thread.
Band3GraphicsSystem* Active();

}  // namespace band3::render::sync_gpu
