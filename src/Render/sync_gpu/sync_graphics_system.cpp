#include "src/Render/sync_gpu/sync_graphics_system.h"

#include <rex/cvar.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory.h>
#include <rex/system/function_dispatcher.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xthread.h>
#include <rex/ui/graphics_provider.h>
#include <rex/ui/presenter.h>
#ifdef _WIN32
#include <rex/ui/d3d12/d3d12_provider.h>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "src/Hooks/frame_pacing.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Render/sync_gpu/sync_monitor.h"
#include "src/settings.h"

// See sync_graphics_system.h.

namespace band3::render::sync_gpu {

// the SDK's status macros name it unqualified
using rex::X_STATUS;

namespace {

std::atomic<Band3GraphicsSystem*> g_active{nullptr};

// The registers the guest may know of, register_table.inc's (an X-macro list
// of XE_GPU_REGISTER(index, type, name)), for the command processor's
// unknown-register counts. Without the file every register counts as known.
#if __has_include(<rex/graphics/register_table.inc>)
#define BAND3_HAVE_REGISTER_TABLE 1
constexpr uint32_t kKnownRegisters[] = {
#define XE_GPU_REGISTER(index, type, name) index,
#include <rex/graphics/register_table.inc>
#undef XE_GPU_REGISTER
};
#else
#define BAND3_HAVE_REGISTER_TABLE 0
#endif

// the guest's GPU registers, as the plugin maps them (n7_design.md section 1)
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x10000;

// Xenia's thread stacks for its GPU threads
constexpr uint32_t kThreadStack = 128 * 1024;

// how long the command processor's thread sleeps with nothing to run before
// it looks again (Xenia's WorkerThreadMain: 100 ms), if no write wakes it
constexpr auto kIdleWait = std::chrono::milliseconds(100);
// how often the vblank thread reads the guest's refresh rate again, runs the
// watchdog and logs the summary
constexpr int64_t kModeReadNs = 1'000'000'000;
constexpr int64_t kWatchNs = 1'000'000'000;
constexpr int64_t kSummaryNs = 10'000'000'000;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// Guest physical memory for the command processor, as Xenia's
// Memory::TranslatePhysical reaches it: the low 29 bits of an address, null
// past the 512 MB.
struct PhysicalMemory final : GuestMemory {
    rex::memory::Memory* memory = nullptr;

    uint8_t* TranslatePhysical(uint32_t address, uint32_t size) override {
        if (!memory) return nullptr;
        const uint32_t physical = address & 0x1FFFFFFF;
        if (uint64_t(physical) + size > 0x20000000) return nullptr;
        return memory->TranslatePhysical(physical);
    }
};

#ifdef _WIN32
// The presenter's device loss (a GPU hang): the plugin rebuilds its device,
// but the SDK's drawers here have nothing to rebuild with, so band3 stops,
// through abort() so the crash trace logs the device's DRED (crash_trace.h).
void OnHostGpuLoss(bool is_responsible, bool statically_from_ui_thread) {
    REXLOG_ERROR(
        "sync gpu: the host GPU's device was lost ({}{}). Without the emulated GPU band3 "
        "can't make it again, so it stops; band3_crash_trace.txt has the device's DRED "
        "(with dred on)",
        is_responsible ? "presenting caused it" : "lost elsewhere",
        statically_from_ui_thread ? ", on the UI thread" : "");
    std::abort();
}
#endif

// the guest's refresh rate, as the game asks for it
double GuestRefreshHz() {
    rex::system::X_VIDEO_MODE mode{};
    rex::kernel::xboxkrnl::VdQueryVideoMode(&mode);
    return double(float(mode.refresh_rate));
}

}  // namespace

struct Band3GraphicsSystem::Impl {
    PhysicalMemory memory;
    SyncCommandProcessor cp;

    // the presenter goes before the provider it was made by
    std::unique_ptr<rex::ui::GraphicsProvider> provider;
    std::unique_ptr<rex::ui::Presenter> presenter;

    rex::runtime::FunctionDispatcher* dispatcher = nullptr;
    rex::system::KernelState* kernel_state = nullptr;
    // VdSetGraphicsInterruptCallback's callback (high half) and its data
    std::atomic<uint64_t> interrupt{0};
    std::atomic<bool> running{false};
    // the vblank every millisecond: the frame cap's ask (cap_free_running) or
    // native_vblank_free_running's, whichever is on; free_running is what the
    // vblank thread reads, set under free_running_mutex so two changes at
    // once can't leave it stale
    std::atomic<bool> free_running{false};
    std::atomic<bool> cap_free_running{false};
    std::mutex free_running_mutex;
    std::atomic<uint64_t> vblanks{0};
    std::atomic<bool> warned_no_thread{false};

    // the command processor's thread sleeps on this until the write pointer moves
    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake = false;

    rex::system::object_ref<rex::system::XHostThread> cp_thread;
    rex::system::object_ref<rex::system::XHostThread> vblank_thread;
    // the command processor's and the vblank thread's handles, for their CPU
    // time (Windows)
    std::atomic<void*> cp_thread_handle{nullptr};
    std::atomic<void*> vblank_thread_handle{nullptr};

    Impl() : cp(memory, MakeHooks(), MakeConfig()) {
        cp.SetQueryLog(REXCVAR_GET(native_query_log));
    }

    ~Impl() {
#ifdef _WIN32
        for (auto* handle : {&cp_thread_handle, &vblank_thread_handle})
            if (void* h = handle->exchange(nullptr)) CloseHandle(static_cast<HANDLE>(h));
#endif
    }

    SyncCpHooks MakeHooks() {
        SyncCpHooks hooks;
        hooks.interrupt = [this](uint32_t source, uint32_t cpu) { DispatchInterrupt(source, cpu); };
        hooks.write_pointer_updated = [this] { Wake(); };
        // A poll that didn't match: Xenia sleeps wait / 0x100 ms for a long
        // wait. std::this_thread::sleep_for on Windows can take a whole
        // system timer tick (15.6 ms) without timeBeginPeriod, which would
        // hold every frame behind the vsync wait; band3's high-resolution
        // timer wakes within a few hundred microseconds. A short wait (under
        // 0x100) yields, as Xenia's does, unless native_sync_short_wait_us
        // asks for a sleep instead (to try against the spin a yield makes).
        hooks.wait = [](uint32_t wait) {
            if (wait >= 0x100) {
                pacing::SleepFor(int64_t(wait / 0x100) * 1'000'000);
            } else if (const int32_t us = REXCVAR_GET(native_sync_short_wait_us); us > 0) {
                pacing::SleepFor(int64_t(us) * 1'000);
            } else {
                std::this_thread::yield();
            }
        };
        // the first swap's words are a layout check and native_query_log's
        // lines were asked for; the rest is something the processor didn't
        // expect
        hooks.log = [](const std::string& line) {
            if (line.find("first XE_SWAP") != std::string::npos ||
                line.starts_with("sync gpu: query log:"))
                REXLOG_INFO("{}", line);
            else
                REXLOG_WARN("{}", line);
        };
        return hooks;
    }

    static SyncCpConfig MakeConfig() {
        SyncCpConfig config;
        config.fake_sample_count = REXCVAR_GET(native_query_sample_count);
        return config;
    }

    // why the vblank is free-running, for the log
    static std::string FreeRunningWhy() {
        return REXCVAR_GET(native_vblank_free_running)
                   ? "(native_vblank_free_running)"
                   : "while the frame cap paces the game";
    }

    // free_running from the frame cap's ask and native_vblank_free_running,
    // logged when it changes
    void ApplyFreeRunning() {
        std::lock_guard lock(free_running_mutex);
        const bool free = REXCVAR_GET(native_vblank_free_running) || cap_free_running.load();
        if (free_running.exchange(free) != free)
            REXLOG_INFO("sync gpu: vblank {}", free ? "free-running (1 ms) " + FreeRunningWhy()
                                                    : std::string("at the guest's refresh rate"));
    }

    void Wake() {
        {
            std::lock_guard lock(wake_mutex);
            wake = true;
        }
        wake_cv.notify_one();
    }

    // Xenia's GraphicsSystem::DispatchInterruptCallback: the guest's handler
    // on this (kernel) thread, as hardware thread `cpu`
    void DispatchInterrupt(uint32_t source, uint32_t cpu) {
        const uint64_t packed = interrupt.load(std::memory_order_acquire);
        const uint32_t callback = uint32_t(packed >> 32);
        if (!callback || !dispatcher) return;
        rex::system::XThread* thread = rex::system::XThread::GetCurrentThread();
        if (!thread) {
            if (!warned_no_thread.exchange(true))
                REXLOG_ERROR("sync gpu: an interrupt off a kernel thread, not dispatched");
            return;
        }
        thread->SetActiveCpu(uint8_t(cpu));
        uint64_t args[] = {source, uint32_t(packed)};
        dispatcher->ExecuteInterrupt(thread->thread_state(), callback, args, std::size(args));
    }

    // Xenia's CommandProcessor::WorkerThreadMain, without its spin: run what
    // the guest wrote, then sleep until it writes more
    int CpMain() {
        cp_thread_handle.store(DuplicateCurrentThread());
        while (running.load(std::memory_order_acquire)) {
            if (cp.ExecutePending()) continue;
            std::unique_lock lock(wake_mutex);
            wake_cv.wait_for(lock, kIdleWait,
                             [this] { return wake || !running.load(std::memory_order_acquire); });
            wake = false;
        }
        return 0;
    }

    // Xenia's vsync worker and MarkVblank, on a fixed beat (FrameCapSchedule)
    // rather than its 1 ms poll; and the watchdog and the summary
    int VblankMain() {
        vblank_thread_handle.store(DuplicateCurrentThread());
        pacing::FrameCapSchedule schedule;
        StallWatch watch;
        double hz = 60;
        int64_t next_mode = 0;
        int64_t next_watch = NowNs() + kWatchNs;
        int64_t summary_start = NowNs();
        int64_t next_summary = summary_start + kSummaryNs;
        SyncCpStats summary_from = cp.stats();
        uint64_t vblanks_from = 0;
        while (running.load(std::memory_order_acquire)) {
            int64_t now = NowNs();
            if (now >= next_mode) {
                const double guest = GuestRefreshHz();
                if (guest != hz && next_mode)
                    REXLOG_INFO("sync gpu: vblank at {:.5g} Hz (was {:.5g})", guest, hz);
                else if (!next_mode)
                    REXLOG_INFO("sync gpu: vblank at {:.5g} Hz{}", guest,
                                free_running.load() ? ", free-running (1 ms) " + FreeRunningWhy()
                                                    : std::string());
                hz = guest;
                next_mode = now + kModeReadNs;
            }
            const bool free = free_running.load(std::memory_order_relaxed);
            schedule.SetPeriod(VblankPeriodNs(hz, free));
            const int64_t go = schedule.Next(now);
            if (go > now) {
                // the vblank paces the game only without the frame cap: then
                // to the beat (a spin at the end); free-running, a sleep will do
                if (free)
                    pacing::SleepFor(go - now);
                else
                    pacing::WaitUntil(go);
            }
            if (!running.load(std::memory_order_acquire)) break;
            cp.IncrementCounter();
            vblanks.fetch_add(1, std::memory_order_relaxed);
            DispatchInterrupt(0, 2);

            now = NowNs();
            if (now >= next_watch) {
                for (const std::string& line : watch.Check(cp.current_wait(), cp.read_index(),
                                                           cp.write_index()))
                    REXLOG_WARN("{}", line);
                next_watch = now + kWatchNs;
            }
            if (now >= next_summary) {
                const SyncCpStats stats = cp.stats();
                const uint64_t v = vblanks.load(std::memory_order_relaxed);
                REXLOG_INFO("{}", SummaryLine(DeltaOf(stats, summary_from),
                                              double(now - summary_start) / 1e9, v - vblanks_from));
                summary_from = stats;
                vblanks_from = v;
                summary_start = now;
                next_summary = now + kSummaryNs;
            }
        }
        return 0;
    }

    // a handle to the calling thread that can read its times, null if none
    // (or off Windows)
    static void* DuplicateCurrentThread() {
#ifdef _WIN32
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                            THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            return self;
#endif
        return nullptr;
    }

    // the CPU time of the thread `handle` holds, -1 if it can't be told
    static double ThreadMs(const std::atomic<void*>& handle) {
#ifdef _WIN32
        HANDLE h = static_cast<HANDLE>(handle.load());
        FILETIME created, exited, kernel, user;
        if (!h || !GetThreadTimes(h, &created, &exited, &kernel, &user)) return -1;
        auto ticks = [](const FILETIME& f) {
            return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        // 100 ns ticks
        return double(ticks(kernel) + ticks(user)) / 1e4;
#else
        (void)handle;
        return -1;
#endif
    }

    static uint32_t ReadThunk(void*, void* context, uint32_t addr) {
        return static_cast<Impl*>(context)->cp.MmioRead(addr);
    }
    static void WriteThunk(void*, void* context, uint32_t addr, uint32_t value) {
        static_cast<Impl*>(context)->cp.MmioWrite(addr, value);
    }

    // a kernel thread running `body`, named, started; null if it couldn't be
    rex::system::object_ref<rex::system::XHostThread> StartThread(const char* name,
                                                                  std::function<int()> body) {
        auto thread = rex::system::object_ref<rex::system::XHostThread>(
            new rex::system::XHostThread(kernel_state, kThreadStack, 0, std::move(body)));
        thread->set_name(name);
        if (XFAILED(thread->Create())) {
            REXLOG_ERROR("sync gpu: couldn't start the {} thread", name);
            return {};
        }
        return thread;
    }
};

Band3GraphicsSystem::Band3GraphicsSystem() : impl_(std::make_unique<Impl>()) {
    g_active.store(this, std::memory_order_release);
    // F4 changes it at once
    rex::cvar::RegisterChangeCallback("native_query_sample_count",
                                      [this](std::string_view, std::string_view) {
                                          impl_->cp.SetFakeSampleCount(
                                              REXCVAR_GET(native_query_sample_count));
                                      });
    // turned on, the next ZPD packets are logged
    rex::cvar::RegisterChangeCallback("native_query_log", [this](std::string_view,
                                                                 std::string_view) {
        impl_->cp.SetQueryLog(REXCVAR_GET(native_query_log));
    });
    // the vblank thread reads it each beat, so a change applies at once
    impl_->ApplyFreeRunning();
    rex::cvar::RegisterChangeCallback("native_vblank_free_running",
                                      [this](std::string_view, std::string_view) {
                                          impl_->ApplyFreeRunning();
                                      });
}

Band3GraphicsSystem::~Band3GraphicsSystem() {
    Shutdown();
    rex::cvar::UnregisterChangeCallbacks("native_query_sample_count");
    rex::cvar::UnregisterChangeCallbacks("native_query_log");
    rex::cvar::UnregisterChangeCallbacks("native_vblank_free_running");
    Band3GraphicsSystem* self = this;
    g_active.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
}

X_STATUS Band3GraphicsSystem::SetupPresentation(rex::ui::WindowedAppContext*) {
    // idempotent, as IGraphicsSystem asks
    if (impl_->presenter) return X_STATUS_SUCCESS;
#ifdef _WIN32
    // what the xenos plugin presents with: the SDK's Direct3D 12 provider and
    // its presenter (n7_design.md decision 3)
    std::unique_ptr<rex::ui::d3d12::D3D12Provider> provider =
        rex::ui::d3d12::D3D12Provider::Create();
    if (!provider) {
        REXLOG_ERROR("sync gpu: couldn't set up Direct3D 12 (see above), so there's nothing to "
                     "present with");
        return X_STATUS_UNSUCCESSFUL;
    }
    std::unique_ptr<rex::ui::Presenter> presenter = provider->CreatePresenter(OnHostGpuLoss);
    if (!presenter) {
        REXLOG_ERROR("sync gpu: couldn't make the Direct3D 12 presenter");
        return X_STATUS_UNSUCCESSFUL;
    }
    impl_->provider = std::move(provider);
    impl_->presenter = std::move(presenter);
    REXLOG_INFO("sync gpu: presenting with the SDK's Direct3D 12 provider and presenter");
    return X_STATUS_SUCCESS;
#else
    // N7-5: the SDK's Vulkan provider, where it exports one
    REXLOG_ERROR("sync gpu: emulated_gpu off presents with Direct3D 12, which this platform "
                 "doesn't have yet; start with emulated_gpu on");
    return X_STATUS_NOT_IMPLEMENTED;
#endif
}

X_STATUS Band3GraphicsSystem::SetupGuestGpu(rex::runtime::FunctionDispatcher* function_dispatcher,
                                            rex::system::KernelState* kernel_state) {
    Impl& s = *impl_;
    if (s.running.load()) return X_STATUS_SUCCESS;
    s.dispatcher = function_dispatcher;
    s.kernel_state = kernel_state;
    s.memory.memory = kernel_state ? kernel_state->memory() : nullptr;
    if (!s.dispatcher || !s.kernel_state || !s.memory.memory) {
        REXLOG_ERROR("sync gpu: no kernel, dispatcher or memory to run the guest's GPU with");
        return X_STATUS_UNSUCCESSFUL;
    }
    if (!s.memory.memory->AddVirtualMappedRange(kMmioBase, kMmioMask, kMmioSize, &s,
                                                &Impl::ReadThunk, &Impl::WriteThunk)) {
        REXLOG_ERROR("sync gpu: couldn't map the GPU's registers at {:08X}", kMmioBase);
        return X_STATUS_UNSUCCESSFUL;
    }
#if BAND3_HAVE_REGISTER_TABLE
    s.cp.SetKnownRegisters(kKnownRegisters);
    REXLOG_INFO("sync gpu: registers at {:08X}, {} known (register_table.inc)", kMmioBase,
                std::size(kKnownRegisters));
#else
    REXLOG_INFO("sync gpu: registers at {:08X} (no register table: none counted unknown)",
                kMmioBase);
#endif
    s.running.store(true, std::memory_order_release);
    s.cp_thread = s.StartThread("band3 GPU sync", [&s] { return s.CpMain(); });
    s.vblank_thread = s.StartThread("band3 GPU vblank", [&s] { return s.VblankMain(); });
    if (!s.cp_thread || !s.vblank_thread) {
        Shutdown();
        return X_STATUS_UNSUCCESSFUL;
    }
    REXLOG_INFO("sync gpu: command processor and vblank threads started (queries answered "
                "with {} samples)",
                REXCVAR_GET(native_query_sample_count));
    return X_STATUS_SUCCESS;
}

bool Band3GraphicsSystem::has_presentation() const { return impl_->presenter != nullptr; }

rex::ui::GraphicsProvider* Band3GraphicsSystem::provider() const { return impl_->provider.get(); }

rex::ui::Presenter* Band3GraphicsSystem::presenter() const { return impl_->presenter.get(); }

void Band3GraphicsSystem::SetInterruptCallback(uint32_t callback, uint32_t user_data) {
    impl_->interrupt.store(uint64_t(callback) << 32 | user_data, std::memory_order_release);
    REXLOG_INFO("sync gpu: interrupt callback {:08X} (data {:08X})", callback, user_data);
}

void Band3GraphicsSystem::InitializeRingBuffer(uint32_t ptr, uint32_t size_log2) {
    impl_->cp.InitializeRingBuffer(ptr, size_log2);
    REXLOG_INFO("sync gpu: ring at {:08X}, {} bytes", ptr, impl_->cp.primary_buffer_size());
}

void Band3GraphicsSystem::EnableReadPointerWriteBack(uint32_t ptr, uint32_t block_size_log2) {
    impl_->cp.EnableReadPointerWriteBack(ptr, block_size_log2);
    REXLOG_INFO("sync gpu: read pointer write-back at {:08X} (block 2^{})", ptr, block_size_log2);
}

void Band3GraphicsSystem::Shutdown() {
    Impl& s = *impl_;
    if (!s.running.exchange(false)) return;
    s.cp.RequestStop();
    s.Wake();
    // as Xenia's GraphicsSystem::Shutdown waits for its threads
    for (auto* thread : {&s.cp_thread, &s.vblank_thread}) {
        if (*thread) {
            (*thread)->Wait(0, 0, 0, nullptr);
            thread->reset();
        }
    }
    REXLOG_INFO("sync gpu: stopped");
}

void Band3GraphicsSystem::SetVblankFreeRunning(bool free_running) {
    impl_->cap_free_running.store(free_running);
    impl_->ApplyFreeRunning();
}

GammaRamp Band3GraphicsSystem::DisplayGamma() const { return impl_->cp.DisplayGamma(); }

SyncGpuStats Band3GraphicsSystem::Stats() const {
    SyncGpuStats s;
    s.cp = impl_->cp.stats();
    s.vblanks = impl_->vblanks.load(std::memory_order_relaxed);
    s.thread_ms = Impl::ThreadMs(impl_->cp_thread_handle);
    s.vblank_thread_ms = Impl::ThreadMs(impl_->vblank_thread_handle);
    return s;
}

Band3GraphicsSystem* Active() { return g_active.load(std::memory_order_acquire); }

}  // namespace band3::render::sync_gpu
