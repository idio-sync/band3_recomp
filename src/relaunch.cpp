#include "relaunch.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <fstream>
#include <thread>
#include <unistd.h>
#endif

#include <chrono>
#include <string>
#include <string_view>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "settings.h"

namespace band3::relaunch {

namespace {

constexpr std::chrono::seconds kWaitLimit{30};

bool g_relaunched = false;

#ifdef _WIN32
// drops each " --name" and " --name=value" from a command line; " --name_more"
// is another setting and stays
void EraseFlag(std::wstring& command, std::wstring_view name) {
    const std::wstring flag = L" --" + std::wstring(name);
    for (size_t from = 0, at; (at = command.find(flag, from)) != std::wstring::npos;) {
        const size_t after = at + flag.size();
        if (after < command.size() && command[after] != L' ' && command[after] != L'=') {
            from = after;
            continue;
        }
        const size_t end = command.find(L' ', after);
        command.erase(at, end == std::wstring::npos ? std::wstring::npos : end - at);
        from = at;
    }
}

std::wstring Widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()),
                                        nullptr, 0);
    std::wstring wide(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(),
                        len);
    return wide;
}
#endif

}

bool StartAgain(const std::vector<std::string>& extra_args) {
#ifdef _WIN32
    // this run's own command line: GetArgs() also has the game arguments band3
    // adds itself (-lang, -define)
    std::wstring command = GetCommandLineW();
    // a run that was itself relaunched has the last one's pid: given twice, the
    // setting isn't read at all
    EraseFlag(command, L"relaunch_wait_pid");
    // the launcher was this run's; the next one starts the game
    EraseFlag(command, L"launcher");
    command += L" --relaunch_wait_pid=" + std::to_wstring(GetCurrentProcessId());
    for (const std::string& arg : extra_args) {
        const std::wstring wide = Widen(arg);
        command += wide.find(L' ') == std::wstring::npos ? L" " + wide : L" \"" + wide + L"\"";
    }

    // the same window state as this run's start: band3ctl starts tests
    // minimized without focus. Not this run's handles.
    STARTUPINFOW startup{};
    GetStartupInfoW(&startup);
    startup.cb = sizeof(startup);
    startup.lpReserved = nullptr;
    startup.lpReserved2 = nullptr;
    startup.cbReserved2 = 0;
    startup.dwFlags &= ~STARTF_USESTDHANDLES;

    PROCESS_INFORMATION process{};
    if (!CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        REXLOG_ERROR("Relaunch: couldn't start band3 again (error {})", GetLastError());
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    REXLOG_INFO("Relaunch: started band3 again (pid {}); this one closes",
                process.dwProcessId);
    return true;
#else
    std::vector<std::string> args;
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    for (std::string arg; std::getline(cmdline, arg, '\0');) {
        // a relaunched run's pid from last time; given twice, it isn't read
        if (arg.starts_with("--relaunch_wait_pid=")) continue;
        // the launcher was this run's; the next one starts the game
        if (arg == "--launcher" || arg.starts_with("--launcher=")) continue;
        args.push_back(std::move(arg));
    }
    if (args.empty()) {
        REXLOG_ERROR("Relaunch: couldn't read this run's command line");
        return false;
    }
    args.push_back("--relaunch_wait_pid=" + std::to_string(getpid()));
    args.insert(args.end(), extra_args.begin(), extra_args.end());
    std::vector<char*> argv;
    for (std::string& arg : args) argv.push_back(arg.data());
    argv.push_back(nullptr);

    const pid_t child = fork();
    if (child < 0) {
        REXLOG_ERROR("Relaunch: couldn't start band3 again (errno {})", errno);
        return false;
    }
    if (child == 0) {
        execv("/proc/self/exe", argv.data());
        _exit(127);
    }
    REXLOG_INFO("Relaunch: started band3 again (pid {}); this one closes", child);
    return true;
#endif
}

void WaitForPrevious() {
    const int32_t pid = REXCVAR_GET(relaunch_wait_pid);
    if (pid <= 0) return;
    g_relaunched = true;
    REXLOG_INFO("Relaunch: waiting for the previous band3 (pid {}) to close", pid);
    bool closed = true;
#ifdef _WIN32
    if (HANDLE previous = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid))) {
        closed = WaitForSingleObject(
                     previous, static_cast<DWORD>(
                                   std::chrono::milliseconds(kWaitLimit).count())) ==
                 WAIT_OBJECT_0;
        CloseHandle(previous);
    }
#else
    const auto end = std::chrono::steady_clock::now() + kWaitLimit;
    while (kill(pid, 0) == 0 || errno != ESRCH) {
        if (std::chrono::steady_clock::now() >= end) {
            closed = false;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
#endif
    if (!closed) REXLOG_WARN("Relaunch: the previous band3 is still running; starting anyway");
    rex::cvar::SetFlagByName("relaunch_wait_pid", "0");
}

bool WasRelaunched() { return g_relaunched; }

}
