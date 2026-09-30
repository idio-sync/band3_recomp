// Experimental: logs a stack trace when the process is about to fast-fail
// through abort(), std::terminate or a CRT invalid parameter, which otherwise
// ends band3 with 0xC0000409 and nothing in the log. Frames are module+RVA;
// resolve them against out/build/<preset>/band3.map.

#ifdef _WIN32

#include <windows.h>

#include <rex/logging.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <typeinfo>

namespace {

void DumpStack(const char* why) {
    void* frames[64];
    const USHORT n = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
    std::string text = std::string("[crash-trace] ") + why + " on thread " +
                       std::to_string(GetCurrentThreadId()) + "\n";
    for (USHORT i = 0; i < n; i++) {
        HMODULE mod = nullptr;
        char name[MAX_PATH] = "?";
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(frames[i]), &mod);
        if (mod) GetModuleFileNameA(mod, name, MAX_PATH);
        const char* base = std::strrchr(name, '\\');
        char line[512];
        std::snprintf(line, sizeof(line), "  #%02u %s+0x%llX\n", i, base ? base + 1 : name,
                      static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                      reinterpret_cast<uintptr_t>(mod)));
        text += line;
    }
    REXLOG_CRITICAL("{}", text);
    if (auto logger = rex::GetLogger()) logger->flush();
    if (FILE* f = std::fopen("band3_crash_trace.txt", "a")) {
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }
}

void OnAbort(int) { DumpStack("abort()"); }

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
    DumpStack(what.c_str());
    std::abort();
}

void OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    DumpStack("CRT invalid parameter");
}

struct Install {
    Install() {
        std::signal(SIGABRT, OnAbort);
        std::set_terminate(OnTerminate);
        _set_invalid_parameter_handler(OnInvalidParameter);
    }
} g_install;

}  // namespace

#endif
