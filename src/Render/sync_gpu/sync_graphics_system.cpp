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
// the provider the SDK's runtime was built with (public defines on
// rex::runtime)
#if defined(REX_HAS_D3D12) && REX_HAS_D3D12
#define BAND3_SYNC_PRESENT_D3D12 1
#define BAND3_SYNC_PRESENT_VULKAN 0
#include <rex/ui/d3d12/d3d12_provider.h>
#elif defined(REX_HAS_VULKAN) && REX_HAS_VULKAN
#define BAND3_SYNC_PRESENT_D3D12 0
#define BAND3_SYNC_PRESENT_VULKAN 1
#include <rex/ui/vulkan/provider.h>
#else
#define BAND3_SYNC_PRESENT_D3D12 0
#define BAND3_SYNC_PRESENT_VULKAN 0
#endif
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
// pthread_getcpuclockid
#include <pthread.h>
#include <time.h>
#define BAND3_THREAD_CPU_CLOCK 1
#endif
#ifndef BAND3_THREAD_CPU_CLOCK
#define BAND3_THREAD_CPU_CLOCK 0
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

// For unknown-register counts; without the file all count as known.
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

// as the plugin maps them
constexpr uint32_t kMmioBase = 0x7FC80000;
constexpr uint32_t kMmioMask = 0xFFFF0000;
constexpr uint32_t kMmioSize = 0x10000;

// Xenia's GPU thread stack size
constexpr uint32_t kThreadStack = 128 * 1024;

// as Xenia's WorkerThreadMain
constexpr auto kIdleWait = std::chrono::milliseconds(100);
// vblank thread: refresh-rate reread, watchdog and summary intervals
constexpr int64_t kModeReadNs = 1'000'000'000;
constexpr int64_t kWatchNs = 1'000'000'000;
constexpr int64_t kSummaryNs = 10'000'000'000;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// As Xenia's Memory::TranslatePhysical: low 29 bits, null past 512 MB.
struct PhysicalMemory final : GuestMemory {
    rex::memory::Memory* memory = nullptr;

    uint8_t* TranslatePhysical(uint32_t address, uint32_t size) override {
        if (!memory) return nullptr;
        const uint32_t physical = address & 0x1FFFFFFF;
        if (uint64_t(physical) + size > 0x20000000) return nullptr;
        return memory->TranslatePhysical(physical);
    }
};

// Unlike the plugin, nothing here can rebuild the device; abort() so the
// crash trace gets DRED (Windows).
[[maybe_unused]] void OnHostGpuLoss(bool is_responsible, bool statically_from_ui_thread) {
#ifdef _WIN32
    constexpr const char* kTrace =
        "; the crash report (logs/crash-*.txt) has the device's DRED (with dred on)";
#else
    constexpr const char* kTrace = "";
#endif
    REXLOG_ERROR("sync gpu: the host GPU's device was lost ({}{}). Without the emulated GPU band3 "
                 "can't make it again, so it stops{}",
                 is_responsible ? "presenting caused it" : "lost elsewhere",
                 statically_from_ui_thread ? ", on the UI thread" : "", kTrace);
    std::abort();
}

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
    // VdSetGraphicsInterruptCallback's callback (high half) and data
    std::atomic<uint64_t> interrupt{0};
    std::atomic<bool> running{false};
    // cap_free_running || native_vblank_free_running, set under
    // free_running_mutex so concurrent changes can't leave it stale
    std::atomic<bool> free_running{false};
    std::atomic<bool> cap_free_running{false};
    std::mutex free_running_mutex;
    std::atomic<uint64_t> vblanks{0};
    std::atomic<bool> warned_no_thread{false};

    std::mutex wake_mutex;
    std::condition_variable wake_cv;
    bool wake = false;

    rex::system::object_ref<rex::system::XHostThread> cp_thread;
    rex::system::object_ref<rex::system::XHostThread> vblank_thread;
    // set by the thread as it starts: a handle (Windows) or CPU clock (Linux)
    struct ThreadClock {
        std::atomic<void*> handle{nullptr};
#if BAND3_THREAD_CPU_CLOCK
        clockid_t clock{};
        // clock is set before this, and never after
        std::atomic<bool> has_clock{false};
#endif
    };
    ThreadClock cp_clock;
    ThreadClock vblank_clock;

    Impl() : cp(memory, MakeHooks(), MakeConfig()) {
        cp.SetQueryLog(REXCVAR_GET(native_query_log));
    }

    ~Impl() {
#ifdef _WIN32
        for (ThreadClock* c : {&cp_clock, &vblank_clock})
            if (void* h = c->handle.exchange(nullptr)) CloseHandle(static_cast<HANDLE>(h));
#endif
    }

    SyncCpHooks MakeHooks() {
        SyncCpHooks hooks;
        hooks.interrupt = [this](uint32_t source, uint32_t cpu) { DispatchInterrupt(source, cpu); };
        hooks.write_pointer_updated = [this] { Wake(); };
        // pacing::SleepFor, not sleep_for: on Windows that can take a whole
        // 15.6 ms timer tick and stall every frame behind the vsync wait.
        // Short waits yield as in Xenia unless native_sync_short_wait_us.
        hooks.wait = [](uint32_t wait) {
            if (wait >= 0x100) {
                pacing::SleepFor(int64_t(wait / 0x100) * 1'000'000);
            } else if (const int32_t us = REXCVAR_GET(native_sync_short_wait_us); us > 0) {
                pacing::SleepFor(int64_t(us) * 1'000);
            } else {
                std::this_thread::yield();
            }
        };
        // all but the first swap and query log lines are unexpected
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

    static std::string FreeRunningWhy() {
        return REXCVAR_GET(native_vblank_free_running)
                   ? "(native_vblank_free_running)"
                   : "while the frame cap paces the game";
    }

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

    // Xenia's GraphicsSystem::DispatchInterruptCallback, on this kernel
    // thread as hardware thread `cpu`
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

    // Xenia's CommandProcessor::WorkerThreadMain, without its spin
    int CpMain() {
        CaptureCurrentThread(cp_clock);
        while (running.load(std::memory_order_acquire)) {
            if (cp.ExecutePending()) continue;
            std::unique_lock lock(wake_mutex);
            wake_cv.wait_for(lock, kIdleWait,
                             [this] { return wake || !running.load(std::memory_order_acquire); });
            wake = false;
        }
        return 0;
    }

    // Xenia's vsync worker and MarkVblank on a fixed beat (FrameCapSchedule)
    // instead of its 1 ms poll; plus the watchdog and summary
    int VblankMain() {
        CaptureCurrentThread(vblank_clock);
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
                // only a pacing vblank needs WaitUntil's precision
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

    static void CaptureCurrentThread(ThreadClock& clock) {
#ifdef _WIN32
        HANDLE self = nullptr;
        if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &self,
                            THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0))
            clock.handle.store(self);
#elif BAND3_THREAD_CPU_CLOCK
        if (pthread_getcpuclockid(pthread_self(), &clock.clock) == 0)
            clock.has_clock.store(true, std::memory_order_release);
#else
        (void)clock;
#endif
    }

    // kernel + user; -1 if unknown
    static double ThreadMs(const ThreadClock& clock) {
#ifdef _WIN32
        HANDLE h = static_cast<HANDLE>(clock.handle.load());
        FILETIME created, exited, kernel, user;
        if (!h || !GetThreadTimes(h, &created, &exited, &kernel, &user)) return -1;
        auto ticks = [](const FILETIME& f) {
            return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
        };
        // 100 ns ticks
        return double(ticks(kernel) + ticks(user)) / 1e4;
#elif BAND3_THREAD_CPU_CLOCK
        // a thread that has exited reads as an error
        timespec t{};
        if (!clock.has_clock.load(std::memory_order_acquire) ||
            clock_gettime(clock.clock, &t) != 0)
            return -1;
        return double(t.tv_sec) * 1e3 + double(t.tv_nsec) / 1e6;
#else
        (void)clock;
        return -1;
#endif
    }

    static uint32_t ReadThunk(void*, void* context, uint32_t addr) {
        return static_cast<Impl*>(context)->cp.MmioRead(addr);
    }
    static void WriteThunk(void*, void* context, uint32_t addr, uint32_t value) {
        static_cast<Impl*>(context)->cp.MmioWrite(addr, value);
    }

    // null on failure
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
    rex::cvar::RegisterChangeCallback("native_query_sample_count",
                                      [this](std::string_view, std::string_view) {
                                          impl_->cp.SetFakeSampleCount(
                                              REXCVAR_GET(native_query_sample_count));
                                      });
    rex::cvar::RegisterChangeCallback("native_query_log", [this](std::string_view,
                                                                 std::string_view) {
        impl_->cp.SetQueryLog(REXCVAR_GET(native_query_log));
    });
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
#if BAND3_SYNC_PRESENT_D3D12
    // what the xenos plugin presents with
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
#elif BAND3_SYNC_PRESENT_VULKAN
    // untested
    std::unique_ptr<rex::ui::vulkan::VulkanProvider> provider =
        rex::ui::vulkan::VulkanProvider::Create(/*with_gpu_emulation=*/false,
                                                /*with_presentation=*/true);
    if (!provider) {
        REXLOG_ERROR("sync gpu: couldn't set up Vulkan (see above), so there's nothing to "
                     "present with");
        return X_STATUS_UNSUCCESSFUL;
    }
    std::unique_ptr<rex::ui::Presenter> presenter = provider->CreatePresenter(OnHostGpuLoss);
    if (!presenter) {
        REXLOG_ERROR("sync gpu: couldn't make the Vulkan presenter");
        return X_STATUS_UNSUCCESSFUL;
    }
    impl_->provider = std::move(provider);
    impl_->presenter = std::move(presenter);
    REXLOG_INFO("sync gpu: presenting with the SDK's Vulkan provider and presenter");
    return X_STATUS_SUCCESS;
#else
    // unreachable: startup keeps the emulated GPU (CanPresentNativeOnly)
    REXLOG_ERROR("sync gpu: this build has neither Direct3D 12 nor Vulkan to present with; "
                 "start with renderer emulated or both");
    return X_STATUS_NOT_IMPLEMENTED;
#endif
}

bool CanPresentNativeOnly() { return BAND3_SYNC_PRESENT_D3D12 || BAND3_SYNC_PRESENT_VULKAN; }

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
    // as Xenia's GraphicsSystem::Shutdown
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
    s.thread_ms = Impl::ThreadMs(impl_->cp_clock);
    s.vblank_thread_ms = Impl::ThreadMs(impl_->vblank_clock);
    return s;
}

Band3GraphicsSystem* Active() { return g_active.load(std::memory_order_acquire); }

}  // namespace band3::render::sync_gpu
