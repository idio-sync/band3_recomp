// crash_trace.cpp's reports off Windows: when a fatal signal ends band3 (a
// segmentation fault outside the guest's memory, a bus error, an illegal
// instruction, an arithmetic error, abort()) or std::terminate does, the
// stack goes to the log and to this run's report, crash-<start>-<pid>.txt in
// the logs folder beside the executable, under the same header as on Windows
// (crash_report.h), and last_crash.txt tells the next start. Frames are
// module+offset ("band3+0x1234"); tools/symbolize.py names band3's with
// addr2line against the build's executable. No minidump: the system's core
// dump, where it keeps them, has every thread.
//
// The signal handlers go in at static initialization, for crashes before the
// runtime is set up. The SDK then takes SIGSEGV and SIGILL for the guest's
// MMIO and GPU write watches, and doesn't hand on a fault its handlers pass
// by, so after that a fault reaches band3 as the last of the SDK's handlers
// (OnSdkException, from WatchGuestFaults). Only the main thread has an
// alternate signal stack, so a stack overflow on another thread ends band3
// without a report.

#ifndef _WIN32

#include "src/crash_trace.h"

#include <dlfcn.h>
#include <execinfo.h>
#include <link.h>
#include <signal.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

#include <SDL3/SDL_messagebox.h>
#include <SDL3/SDL_misc.h>
#include <SDL3/SDL_stdinc.h>

#include <rex/exception_handler.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "src/build_tag.h"
#include "src/crash_report.h"
#include "src/settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <mutex>
#include <string>
#include <typeinfo>

namespace {

// This run's report file and what its header says, fixed when band3 starts
// (SetUpReportFile), so a crash only has to write.
struct ReportFile {
    std::filesystem::path folder;  // <the executable's folder>/logs
    std::filesystem::path path;
    band3::crash_report::Utc started;
    std::string exe;  // ElfExeId
    bool header_written = false;
    bool fatal_recorded = false;
};
ReportFile g_report;
std::mutex g_report_mutex;

band3::crash_report::Utc UtcNow() {
    const time_t now = time(nullptr);
    tm t{};
    gmtime_r(&now, &t);
    return {t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec};
}

unsigned ThreadId() { return unsigned(syscall(SYS_gettid)); }

FILE* Open(const std::filesystem::path& path, const char* mode) {
    return std::fopen(path.c_str(), mode);
}

// the executable's GNU build id, in hex: the first object dl_iterate_phdr
// visits is the executable
int ReadBuildId(dl_phdr_info* info, size_t, void* out) {
    auto* id = static_cast<std::string*>(out);
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr)& ph = info->dlpi_phdr[i];
        if (ph.p_type != PT_NOTE) continue;
        const auto* p = reinterpret_cast<const uint8_t*>(info->dlpi_addr + ph.p_vaddr);
        const uint8_t* end = p + ph.p_memsz;
        while (p + sizeof(ElfW(Nhdr)) <= end) {
            const auto* note = reinterpret_cast<const ElfW(Nhdr)*>(p);
            const uint8_t* name = p + sizeof(*note);
            const uint8_t* desc = name + ((note->n_namesz + 3) & ~3u);
            if (note->n_type == NT_GNU_BUILD_ID && note->n_namesz == 4 &&
                std::memcmp(name, "GNU", 4) == 0) {
                for (uint32_t j = 0; j < note->n_descsz; j++) {
                    char hex[3];
                    std::snprintf(hex, sizeof(hex), "%02x", desc[j]);
                    *id += hex;
                }
                return 1;
            }
            p = desc + ((note->n_descsz + 3) & ~3u);
        }
    }
    return 1;
}

void SetUpReportFile() {
    std::error_code ec;
    const auto exe = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (ec) return;
    g_report.folder = exe.parent_path() / "logs";
    g_report.started = UtcNow();
    g_report.path = g_report.folder / band3::crash_report::ReportFileName(
                                          g_report.started, uint32_t(getpid()));
    std::string id;
    dl_iterate_phdr(ReadBuildId, &id);
    g_report.exe = band3::crash_report::ElfExeId(id);
}

// appends to this run's report, the header first; try_lock, so a crash while
// another report is being written doesn't deadlock
bool WriteReport(const std::string& text) {
    std::unique_lock lock(g_report_mutex, std::try_to_lock);
    if (!lock || g_report.path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(g_report.folder, ec);
    FILE* f = Open(g_report.path, "ab");
    if (!f) return false;
    if (!g_report.header_written) {
        band3::crash_report::Run run;
        run.build = band3::BuildTag();
        run.exe = g_report.exe;
        run.started = g_report.started;
        run.pid = uint32_t(getpid());
        run.renderer = REXCVAR_GET(renderer);
        std::fputs(band3::crash_report::FormatHeader(run).c_str(), f);
        g_report.header_written = true;
    }
    std::fputs(text.c_str(), f);
    std::fclose(f);
    return true;
}

// what ended this run, for the next start's notice: the first one, since
// std::terminate goes on to abort()
void RecordFatal(band3::crash_report::Kind kind) {
    std::unique_lock lock(g_report_mutex, std::try_to_lock);
    if (!lock || g_report.path.empty() || g_report.fatal_recorded) return;
    g_report.fatal_recorded = true;
    band3::crash_report::LastCrash c;
    c.report = g_report.path.string();
    c.kind = kind;
    c.renderer = REXCVAR_GET(renderer);
    c.build = band3::BuildTag();
    if (FILE* f = Open(g_report.folder / "last_crash.txt", "wb")) {
        std::fputs(band3::crash_report::FormatLastCrash(c).c_str(), f);
        std::fclose(f);
    }
}

// this thread's stack as module+offset, one frame a line, from `skip` frames
// in, or from the frame at `from` (a signal's faulting instruction: backtrace
// goes through the signal handler's frame to the code that faulted)
std::string Frames(int skip, uintptr_t from = 0) {
    void* frames[64];
    const int n = backtrace(frames, 64);
    for (int i = skip; from && i < n; i++) {
        if (reinterpret_cast<uintptr_t>(frames[i]) == from) {
            skip = i;
            break;
        }
    }
    std::string text;
    for (int i = skip; i < n; i++) {
        Dl_info dl{};
        const char* module = "?";
        uintptr_t base = 0;
        if (dladdr(frames[i], &dl) && dl.dli_fname) {
            const char* slash = std::strrchr(dl.dli_fname, '/');
            module = slash ? slash + 1 : dl.dli_fname;
            base = reinterpret_cast<uintptr_t>(dl.dli_fbase);
        }
        char line[512];
        const uintptr_t pc = reinterpret_cast<uintptr_t>(frames[i]);
        std::snprintf(line, sizeof(line), "  #%02d %s+0x%llX\n", i - skip, module,
                      static_cast<unsigned long long>(pc - base));
        text += line;
    }
    return text;
}

// writes the stack to the log and this run's report: from the frame at `from`
// if it's in it, else from DumpStack's caller
void DumpStack(const std::string& why, uintptr_t from = 0) {
    std::string text = "[crash-trace] " + why + " on thread " + std::to_string(ThreadId()) +
                       " at " + band3::crash_report::FormatUtc(UtcNow()) + " UTC\n" +
                       Frames(2, from);
    REXLOG_CRITICAL("{}", text);
    if (WriteReport(text)) REXLOG_CRITICAL("Crash report: {}", g_report.path.string());
    if (auto logger = rex::GetLogger()) logger->flush();
}

std::string DescribeSignal(int sig, const siginfo_t* info) {
    std::string text;
    switch (sig) {
        case SIGSEGV: text = "segmentation fault (SIGSEGV)"; break;
        case SIGBUS: text = "bus error (SIGBUS)"; break;
        case SIGILL: text = "illegal instruction (SIGILL)"; break;
        case SIGFPE: text = "arithmetic error (SIGFPE)"; break;
        case SIGABRT: return "abort()";
        default: text = "signal " + std::to_string(sig); break;
    }
    if ((sig == SIGSEGV || sig == SIGBUS) && info) {
        const char* why = sig == SIGSEGV && info->si_code == SEGV_MAPERR   ? "nothing mapped at "
                          : sig == SIGSEGV && info->si_code == SEGV_ACCERR ? "no access to "
                                                                           : "at ";
        char where[64];
        std::snprintf(where, sizeof(where), ", %s0x%016llX", why,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->si_addr)));
        text += where;
    }
    return text;
}

constexpr int kFatalSignals[] = {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT};
std::atomic<unsigned> g_crashing_thread{0};
std::atomic<bool> g_crash_written{false};

// The run's first crash: its report, from the frame at `pc` if it's given, and
// last_crash.txt. A crash on another thread meanwhile waits for it, so band3
// doesn't end halfway through.
void ReportCrash(const std::string& why, uintptr_t pc, band3::crash_report::Kind kind) {
    unsigned expected = 0;
    if (g_crashing_thread.compare_exchange_strong(expected, ThreadId())) {
        DumpStack(why, pc);
        RecordFatal(kind);
        g_crash_written = true;
    } else if (expected != ThreadId()) {
        for (int i = 0; i < 1000 && !g_crash_written; i++) usleep(10000);
    }
}

void OnFatalSignal(int sig, siginfo_t* info, void* context) {
    // a fault's frames from where it happened; abort()'s from the handler's caller
    const auto* uc = static_cast<const ucontext_t*>(context);
    const uintptr_t pc = sig != SIGABRT && uc ? uintptr_t(uc->uc_mcontext.gregs[REG_RIP]) : 0;
    ReportCrash(DescribeSignal(sig, info), pc,
                sig == SIGABRT ? band3::crash_report::Kind::kAbort
                               : band3::crash_report::Kind::kException);
    // the default action, a core dump where the system keeps them: abort()
    // raises again, and a fault happens again once this returns
    signal(sig, SIG_DFL);
}

void OnTerminate() {
    std::string what = "std::terminate";
    if (auto e = std::current_exception()) {
        try {
            std::rethrow_exception(e);
        } catch (const std::exception& ex) {
            what += std::string(" (uncaught ") + typeid(ex).name() + ": " + ex.what() + ")";
        } catch (...) {
            what += " (uncaught non-std exception)";
        }
    }
    DumpStack(what);
    RecordFatal(band3::crash_report::Kind::kTerminate);
    std::abort();
}

// An exception the SDK hands its handlers, last of them. Once set up the SDK
// takes SIGSEGV and SIGILL, and doesn't hand one its handlers pass by on to
// OnFatalSignal, so this is where a crash shows. A guest access violation is
// logged with the host frames it happened in, which tools/symbolize.py names
// (the recompiled functions are named as in band3_functions.toml): the game may
// handle it. Any other is band3's crash.
bool OnSdkException(rex::arch::Exception* ex, void*) {
    using rex::arch::Exception;
    const bool access = ex->code() == Exception::Code::kAccessViolation;
    const uint64_t fault = access ? ex->fault_address() : 0;
    const auto operation = access ? ex->access_violation_operation()
                                  : Exception::AccessViolationOperation::kUnknown;
    const char* op = operation == Exception::AccessViolationOperation::kWrite  ? "write of"
                     : operation == Exception::AccessViolationOperation::kRead ? "read of"
                                                                               : "access to";
    if (auto* memory = access ? rex::system::kernel_memory() : nullptr) {
        const uint64_t membase = reinterpret_cast<uint64_t>(memory->virtual_membase());
        // the guest's address space and the physical memory mapped after it
        if (fault >= membase && fault - membase < (uint64_t(2) << 32)) {
            // the first few: one the game catches itself comes on every boot
            static std::atomic<int> reports{0};
            if (reports++ >= 8) return false;
            REXLOG_ERROR("Guest fault: {} guest 0x{:08X} on thread {}; the host frames it was "
                         "in, innermost first (tools/symbolize.py names them):\n{}",
                         op, static_cast<uint32_t>(fault - membase), ThreadId(),
                         Frames(1, ex->pc()));
            if (auto logger = rex::GetLogger()) logger->flush();
            return false;  // not handled: the game's own handler, if any, or the crash
        }
    }
    char why[96];
    if (access) {
        std::snprintf(why, sizeof(why), "segmentation fault (SIGSEGV), %s 0x%016llX", op,
                      static_cast<unsigned long long>(fault));
    } else {
        std::snprintf(why, sizeof(why), "illegal instruction (SIGILL)");
    }
    ReportCrash(why, ex->pc(), band3::crash_report::Kind::kException);
    // the default action, a core dump where the system keeps them, when it
    // happens again: in place of the SDK's handler
    signal(access ? SIGSEGV : SIGILL, SIG_DFL);
    return false;
}

// BAND3_CRASH_TEST's: past the end of a stack
int Recurse(int depth) {
    volatile char frame[4096];
    frame[0] = char(depth);
    return depth > 0 ? Recurse(depth + 1) + frame[0] : 0;
}

// the main thread's alternate signal stack, for its stack overflow
alignas(16) char g_signal_stack[64 * 1024];

struct Install {
    Install() {
        SetUpReportFile();
        stack_t stack{};
        stack.ss_sp = g_signal_stack;
        stack.ss_size = sizeof(g_signal_stack);
        sigaltstack(&stack, nullptr);
        struct sigaction action{};
        action.sa_sigaction = OnFatalSignal;
        action.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigemptyset(&action.sa_mask);
        for (int sig : kFatalSignals) sigaction(sig, &action, nullptr);
        std::set_terminate(OnTerminate);
    }
} g_install;

}  // namespace

namespace band3::crash_trace {

void WatchGuestFaults() { rex::arch::ExceptionHandler::Install(OnSdkException, nullptr); }

void RunCrashTest() {
    const char* test = std::getenv("BAND3_CRASH_TEST");
    if (!test || !*test) return;
    REXLOG_WARN("BAND3_CRASH_TEST={}: crashing on purpose", test);
    if (std::strcmp(test, "abort") == 0) std::abort();
    if (std::strcmp(test, "terminate") == 0) std::terminate();
    if (std::strcmp(test, "access-violation") == 0) {
        int* volatile nowhere = nullptr;
        *nowhere = 1;
    }
    if (std::strcmp(test, "stack-overflow") == 0) Recurse(1);
    REXLOG_WARN("BAND3_CRASH_TEST: unknown test (abort, terminate, access-violation or "
                "stack-overflow)");
}

void ShowLastCrashNotice() {
    const auto marker = g_report.folder / "last_crash.txt";
    FILE* f = Open(marker, "rb");
    if (!f) return;
    std::string text;
    char buf[4096];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);
    // once: the notice is for the next start only
    std::error_code ec;
    std::filesystem::remove(marker, ec);
    const auto crash = band3::crash_report::ParseLastCrash(text);
    if (!crash) return;
    REXLOG_WARN("The last run crashed; its report: {}", crash->report);
    const std::string notice = band3::crash_report::Notice(*crash);
    // SDL's message box needs no SDL_Init or window
    const SDL_MessageBoxButtonData buttons[] = {
        {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0,
         "OK"},
        {0, 1, "Open folder"},
    };
    const SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, nullptr, "band3", notice.c_str(),
                                 SDL_arraysize(buttons), buttons, nullptr};
    int chosen = 0;
    if (SDL_ShowMessageBox(&box, &chosen) && chosen == 1) {
        SDL_OpenURL(("file://" + g_report.folder.string()).c_str());
    }
}

}  // namespace band3::crash_trace

#endif
