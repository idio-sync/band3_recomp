#include "src/stall_watch.h"

#include "src/Render/sync_gpu/native_only.h"
#include "src/settings.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <initializer_list>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// See stall_watch.h.

namespace band3::stall_watch {
namespace {

using namespace std::chrono_literals;

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// the game thread's marks in its last frame (steady-clock nanoseconds), and
// its frames so far
std::atomic<int64_t> g_begin{0}, g_present{0}, g_finish{0}, g_pace{0}, g_end{0};
std::atomic<uint64_t> g_frames{0};
// the native renderer's worker: what it's doing, since when
std::atomic<int> g_worker{0};
std::atomic<int64_t> g_worker_since{0};

constexpr auto kPoll = 10ms;
constexpr int64_t kSampleEveryNs = 50'000'000;
constexpr int64_t kLogEveryNs = 2'000'000'000;
constexpr int kMaxFrames = 24;   // of a stack
constexpr int kMaxSamples = 16;  // of each thread's stacks in a stall

const char* WorkerName(int w) {
    switch (Worker(w)) {
    case Worker::kWaiting: return "waiting";
    case Worker::kRecording: return "recording a frame";
    case Worker::kGpuWait: return "waiting for the GPU";
    case Worker::kIdle: break;
    }
    return "idle";
}

struct Stack {
    uint64_t pc[kMaxFrames];
    int n = 0;
};

// what's cheap to know about the process at a moment
struct Snapshot {
    int64_t ns = 0;
    double game_cpu_ms = -1, cp_cpu_ms = -1;
    uint64_t read_bytes = 0, write_bytes = 0, read_ops = 0, write_ops = 0, other_ops = 0;
    uint64_t page_faults = 0;
    uint64_t private_mb = 0;
    // the game thread game_cpu_ms is of (it moves at boot)
    const void* game_thread = nullptr;
    int worker = 0;
    int64_t worker_since = 0;
};

// a stall's stack samples, by thread
struct Samples {
    std::vector<Stack> game, cp, worker;
};

// a frame that ran long, as FrameEnd hands it over
struct LongFrame {
    uint64_t frame = 0;
    int64_t start = 0, begin = 0, present = 0, finish = 0, pace = 0, end = 0;
};

#ifdef _WIN32
// a thread to sample, and its stack's range (its TEB's NT_TIB)
struct Sampled {
    HANDLE handle = nullptr;
    uint64_t stack_lo = 0, stack_hi = 0;
};

// NtQueryInformationThread's ThreadBasicInformation, which has the TEB
struct BasicInfo {
    LONG exit_status;
    PVOID teb;
    HANDLE process, thread;
    ULONG_PTR affinity;
    LONG priority, base_priority;
};
using QueryThread = LONG(WINAPI*)(HANDLE, int, PVOID, ULONG, PULONG);

// the stack's range now: its base and the limit committed so far
bool StackRange(Sampled& t) {
    static const auto query = reinterpret_cast<QueryThread>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread")));
    BasicInfo info{};
    if (!query || query(t.handle, 0, &info, sizeof(info), nullptr) != 0 || !info.teb) return false;
    const NT_TIB* tib = static_cast<const NT_TIB*>(info.teb);
    t.stack_lo = reinterpret_cast<uint64_t>(tib->StackLimit);
    t.stack_hi = reinterpret_cast<uint64_t>(tib->StackBase);
    return t.stack_lo < t.stack_hi;
}

// The thread's stack, unwound by its functions' unwind data while it's
// suspended; nothing here allocates (the thread may hold the heap's lock),
// and a frame without unwind data (a leaf: its return address on top) is
// read only inside the stack's range.
Stack SampleStack(Sampled& t) {
    Stack s;
    if (!t.handle || !StackRange(t)) return s;
    if (SuspendThread(t.handle) == DWORD(-1)) return s;
    CONTEXT ctx{};
    ctx.ContextFlags = CONTEXT_FULL;
    if (GetThreadContext(t.handle, &ctx)) {
        while (s.n < kMaxFrames) {
            s.pc[s.n++] = ctx.Rip;
            DWORD64 image = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image, nullptr);
            if (fn) {
                PVOID handler_data = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, image, ctx.Rip, fn, &ctx, &handler_data,
                                 &establisher, nullptr);
            } else {
                if (ctx.Rsp < t.stack_lo || ctx.Rsp + 8 > t.stack_hi) break;
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
            if (!ctx.Rip || ctx.Rsp < t.stack_lo || ctx.Rsp >= t.stack_hi) break;
        }
    }
    ResumeThread(t.handle);
    return s;
}

double CpuMs(HANDLE thread) {
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user)) return -1;
    auto ticks = [](const FILETIME& f) {
        return (uint64_t(f.dwHighDateTime) << 32) | f.dwLowDateTime;
    };
    return double(ticks(kernel) + ticks(user)) / 1e4;
}

// the emulated GPU's command processor thread, by the name the SDK gives it
// (as test_server.cpp's CpThreadMs finds it), or with renderer native the
// sync-only GPU's (sync_graphics_system.h); or null
HANDLE OpenCpThread() {
    const wchar_t* const cp_name =
        band3::render::sync_gpu::NativeOnly() ? L"band3 GPU sync" : L"GPU Commands";
    using GetDescription = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    const auto get_description = reinterpret_cast<GetDescription>(reinterpret_cast<void*>(
        GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription")));
    HANDLE snap =
        get_description ? CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0) : INVALID_HANDLE_VALUE;
    if (snap == INVALID_HANDLE_VALUE) return nullptr;
    HANDLE found = nullptr;
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok && !found; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) continue;
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME |
                                  THREAD_GET_CONTEXT,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        PWSTR name = nullptr;
        if (SUCCEEDED(get_description(h, &name)) && name) {
            if (std::wcsstr(name, cp_name)) found = h;
            LocalFree(name);
        }
        if (found != h) CloseHandle(h);
    }
    CloseHandle(snap);
    return found;
}

// module+RVA, as crash_trace.cpp writes its frames
std::string Frame(uint64_t pc) {
    HMODULE mod = nullptr;
    char name[MAX_PATH] = "?";
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCSTR>(pc), &mod);
    if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
    const char* base = std::strrchr(name, '\\');
    char out[MAX_PATH + 32];
    std::snprintf(out, sizeof(out), "%s+0x%llX", base ? base + 1 : name,
                  static_cast<unsigned long long>(pc - reinterpret_cast<uint64_t>(mod)));
    return out;
}
#endif

class Watcher {
 public:
    static Watcher& Get() {
        // never destroyed: an exit that skips Stop mustn't destroy a running
        // thread
        static Watcher* w = new Watcher;
        return *w;
    }

    void EnsureStarted() {
        if (started_.load(std::memory_order_relaxed)) return;
        std::lock_guard lock(mutex_);
        if (started_.load() || stopped_) return;
        thread_ = std::thread([this] { Run(); });
        started_.store(true);
    }

    void Stop() {
        std::thread t;
        {
            std::lock_guard lock(mutex_);
            stopped_ = true;
            t = std::move(thread_);
        }
        cv_.notify_all();
        if (t.joinable()) t.join();
#ifdef _WIN32
        for (HANDLE h : {game_.handle, cp_.handle, worker_.handle})
            if (h) CloseHandle(h);
        for (HANDLE h : retired_) CloseHandle(h);
        retired_.clear();
        game_.handle = cp_.handle = worker_.handle = nullptr;
#endif
    }

#ifdef _WIN32
    // the game thread, from its own frame (it moves from RB3's splash thread
    // to its main thread at boot), and the native renderer's worker, from
    // its own start; a handle replaced is kept open until Stop, as a sample
    // may be using it
    void SetThread(bool worker) {
        HANDLE h = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME |
                                  THREAD_GET_CONTEXT,
                              FALSE, GetCurrentThreadId());
        std::lock_guard lock(mutex_);
        Sampled& t = worker ? worker_ : game_;
        if (t.handle) retired_.push_back(t.handle);
        t.handle = h;
    }
#endif

    void Ended(const LongFrame& f) {
        {
            std::lock_guard lock(mutex_);
            ended_ = f;
            has_ended_ = true;
        }
        cv_.notify_all();
    }

 private:
    void Run() {
        bool watching = false;
        int64_t watched = 0, next_sample = 0, last_log = 0;
        uint32_t left_out = 0;
        Snapshot at_start;
        Samples samples;
        while (true) {
            LongFrame ended;
            bool has_ended = false;
            {
                std::unique_lock lock(mutex_);
                cv_.wait_for(lock, kPoll, [this] { return stopped_ || has_ended_; });
                if (stopped_) return;
                has_ended = std::exchange(has_ended_, false);
                ended = ended_;
            }
            const int32_t threshold_ms = REXCVAR_GET(game_stall_log_ms);
            if (threshold_ms <= 0) {
                watching = false;
                continue;
            }
            const int64_t now = NowNs();
            const int64_t end = g_end.load(std::memory_order_relaxed);
            // a frame past half the threshold: what's known as it starts
            // being watched, and its stacks from then on, kept if it ends
            // past the threshold (the watcher's look every kPoll would miss
            // one that only just does)
            if (!watching && !has_ended && end && now - end > int64_t(threshold_ms) * 500'000) {
                watching = true;
                watched = end;
                at_start = Take(now);
                samples.game.clear();
                samples.cp.clear();
                samples.worker.clear();
                next_sample = now;
            }
            if (watching && !has_ended && end == watched && now >= next_sample &&
                samples.game.size() < size_t(kMaxSamples)) {
                Sample(samples);
                next_sample = now + kSampleEveryNs;
            }
            if (!has_ended) {
                // the frame watched ended under the threshold
                if (watching && end != watched) watching = false;
                continue;
            }
            const bool matched = watching && ended.start == watched;
            watching = false;
            if (last_log && now - last_log < kLogEveryNs) {
                left_out++;
                continue;
            }
            last_log = now;
            Log(ended, matched ? &at_start : nullptr, matched ? Take(now) : Snapshot{},
                samples, left_out);
            left_out = 0;
        }
    }

    Snapshot Take(int64_t now) {
        Snapshot s;
        s.ns = now;
        s.worker = g_worker.load(std::memory_order_relaxed);
        s.worker_since = g_worker_since.load(std::memory_order_relaxed);
#ifdef _WIN32
        {
            std::lock_guard lock(mutex_);
            s.game_cpu_ms = CpuMs(game_.handle);
            s.game_thread = game_.handle;
        }
        if (!cp_tried_) {
            cp_tried_ = true;
            cp_.handle = OpenCpThread();
        }
        s.cp_cpu_ms = CpuMs(cp_.handle);
        IO_COUNTERS io{};
        if (GetProcessIoCounters(GetCurrentProcess(), &io)) {
            s.read_bytes = io.ReadTransferCount;
            s.write_bytes = io.WriteTransferCount;
            s.read_ops = io.ReadOperationCount;
            s.write_ops = io.WriteOperationCount;
            s.other_ops = io.OtherOperationCount;
        }
        PROCESS_MEMORY_COUNTERS_EX mem{};
        mem.cb = sizeof(mem);
        if (K32GetProcessMemoryInfo(GetCurrentProcess(),
                                    reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&mem), sizeof(mem))) {
            s.page_faults = mem.PageFaultCount;
            s.private_mb = mem.PrivateUsage >> 20;
        }
#endif
        return s;
    }

    void Sample(Samples& samples) {
#ifdef _WIN32
        Sampled game, worker;
        {
            std::lock_guard lock(mutex_);
            game.handle = game_.handle;
            worker.handle = worker_.handle;
        }
        samples.game.push_back(SampleStack(game));
        if (cp_.handle) samples.cp.push_back(SampleStack(cp_));
        if (worker.handle) samples.worker.push_back(SampleStack(worker));
#else
        (void)samples;
#endif
    }

    // the distinct stacks among `stacks`, most seen first
    static std::string Stacks(const char* thread, const std::vector<Stack>& stacks) {
        std::string out;
#ifdef _WIN32
        std::vector<std::pair<int, const Stack*>> distinct;
        for (const Stack& s : stacks) {
            bool found = false;
            for (auto& [n, d] : distinct) {
                if (d->n == s.n && std::memcmp(d->pc, s.pc, sizeof(uint64_t) * s.n) == 0) {
                    n++;
                    found = true;
                    break;
                }
            }
            if (!found) distinct.emplace_back(1, &s);
        }
        std::stable_sort(distinct.begin(), distinct.end(),
                         [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& [n, s] : distinct) {
            char head[96];
            std::snprintf(head, sizeof(head), "\n  %s, %d of %zu samples:", thread, n,
                          stacks.size());
            out += head;
            for (int i = 0; i < s->n; i++) out += (i ? " < " : " ") + Frame(s->pc[i]);
        }
#else
        (void)thread;
        (void)stacks;
#endif
        return out;
    }

    void Log(const LongFrame& f, const Snapshot* from, const Snapshot& to,
             const Samples& samples, uint32_t left_out) {
        auto ms = [](int64_t a, int64_t b) { return a && b && b >= a ? (b - a) / 1e6 : 0.0; };
        char buf[768];
        std::snprintf(buf, sizeof(buf),
                      "game stall: frame %llu took %.1f ms (game %.1f, its Present %.1f, capture "
                      "%.1f, frame cap %.1f, after %.1f)",
                      static_cast<unsigned long long>(f.frame), ms(f.start, f.end),
                      ms(f.start, f.begin), ms(f.begin, f.present), ms(f.present, f.finish),
                      ms(f.finish, f.pace), ms(f.pace, f.end));
        std::string text = buf;
        if (from) {
            // -1: unknown, or another thread's by the end
            auto cpu = [](double a, double b) { return a >= 0 && b >= 0 ? b - a : -1.0; };
            const double game_cpu = from->game_thread == to.game_thread
                                        ? cpu(from->game_cpu_ms, to.game_cpu_ms)
                                        : -1.0;
            std::snprintf(
                buf, sizeof(buf),
                "; over its last %.1f ms: game thread CPU %.1f ms, GPU commands thread CPU "
                "%.1f ms, read %llu KB in %llu ops, wrote %llu KB in %llu ops, %llu other I/O, "
                "%llu page faults, %llu MB private; the native renderer's worker %s for %.1f "
                "ms as it started, %s now",
                (to.ns - from->ns) / 1e6, game_cpu,
                cpu(from->cp_cpu_ms, to.cp_cpu_ms),
                static_cast<unsigned long long>((to.read_bytes - from->read_bytes) >> 10),
                static_cast<unsigned long long>(to.read_ops - from->read_ops),
                static_cast<unsigned long long>((to.write_bytes - from->write_bytes) >> 10),
                static_cast<unsigned long long>(to.write_ops - from->write_ops),
                static_cast<unsigned long long>(to.other_ops - from->other_ops),
                static_cast<unsigned long long>(to.page_faults - from->page_faults),
                static_cast<unsigned long long>(to.private_mb), WorkerName(from->worker),
                from->worker_since ? (from->ns - from->worker_since) / 1e6 : 0.0,
                WorkerName(to.worker));
            text += buf;
            text += Stacks("game thread", samples.game);
            text += Stacks("GPU commands thread", samples.cp);
            text += Stacks("native renderer's worker", samples.worker);
        } else {
            text += "; not watched (the watcher missed it)";
        }
        if (left_out) {
            std::snprintf(buf, sizeof(buf), "\n  (%u more since the last one logged)", left_out);
            text += buf;
        }
        REXLOG_WARN("{}", text);
    }

    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::atomic<bool> started_{false};
    bool stopped_ = false;
    LongFrame ended_;
    bool has_ended_ = false;
#ifdef _WIN32
    Sampled game_, worker_;  // under mutex_
    std::vector<HANDLE> retired_;
    Sampled cp_;    // the watcher's
    bool cp_tried_ = false;
#endif
};

}  // namespace

void PresentBegin() { g_begin.store(NowNs(), std::memory_order_relaxed); }
void PresentDone() { g_present.store(NowNs(), std::memory_order_relaxed); }
void FinishDone() { g_finish.store(NowNs(), std::memory_order_relaxed); }
void PaceDone() { g_pace.store(NowNs(), std::memory_order_relaxed); }

void FrameEnd() {
    const int64_t now = NowNs();
    const int64_t start = g_end.exchange(now, std::memory_order_relaxed);
    const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    const int32_t threshold_ms = REXCVAR_GET(game_stall_log_ms);
    if (threshold_ms <= 0) return;
    Watcher& w = Watcher::Get();
    w.EnsureStarted();
#ifdef _WIN32
    static std::atomic<DWORD> game_tid{0};
    if (const DWORD tid = GetCurrentThreadId(); tid != game_tid.load(std::memory_order_relaxed)) {
        game_tid.store(tid, std::memory_order_relaxed);
        w.SetThread(false);
    }
#endif
    if (!start || now - start <= int64_t(threshold_ms) * 1'000'000) return;
    LongFrame f;
    f.frame = frame;
    f.start = start;
    f.begin = g_begin.load(std::memory_order_relaxed);
    f.present = g_present.load(std::memory_order_relaxed);
    f.finish = g_finish.load(std::memory_order_relaxed);
    f.pace = g_pace.load(std::memory_order_relaxed);
    f.end = now;
    w.Ended(f);
}

void SetWorker(Worker w) {
    if (g_worker.exchange(int(w), std::memory_order_relaxed) != int(w))
        g_worker_since.store(NowNs(), std::memory_order_relaxed);
}

void SetWorkerThread() {
#ifdef _WIN32
    Watcher::Get().SetThread(true);
#endif
}

void Stop() { Watcher::Get().Stop(); }

}  // namespace band3::stall_watch
