#include "src/Video/child_process.h"

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
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace band3::video {

namespace {

// splits what's read into lines, keeping a part line for the next read;
// \r\n ends one, and \r alone ends one too (progress written over itself)
class Lines {
public:
    explicit Lines(const std::function<void(std::string_view)>& on_line) : on_line_(on_line) {}
    void Add(const char* data, size_t n) {
        for (size_t i = 0; i < n; i++) {
            const char c = data[i];
            if (c == '\n' && after_cr_) {
                after_cr_ = false;
            } else if (c == '\n' || c == '\r') {
                on_line_(part_);
                part_.clear();
                after_cr_ = c == '\r';
            } else {
                part_ += c;
                after_cr_ = false;
            }
        }
    }
    void End() {
        if (!part_.empty()) on_line_(part_);
        part_.clear();
    }

private:
    const std::function<void(std::string_view)>& on_line_;
    std::string part_;
    bool after_cr_ = false;
};

#ifdef _WIN32
std::wstring Widen(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int len = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), nullptr, 0);
    std::wstring wide(size_t(len), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), int(utf8.size()), wide.data(), len);
    return wide;
}
#endif

}

std::string QuoteWindowsArgument(std::string_view arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string_view::npos)
        return std::string(arg);
    std::string out = "\"";
    size_t backslashes = 0;
    for (const char c : arg) {
        if (c == '\\') {
            backslashes++;
            continue;
        }
        // backslashes before a quote are doubled, and the quote escaped
        out.append(c == '"' ? backslashes * 2 + 1 : backslashes, '\\');
        backslashes = 0;
        out += c;
    }
    // and before the closing quote
    out.append(backslashes * 2, '\\');
    return out + '"';
}

ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args,
                         const std::function<void(std::string_view)>& on_line,
                         const std::atomic<bool>* cancel) {
    ProcessResult result;
    Lines lines(on_line);
#ifdef _WIN32
    const std::u8string program_u8 = program.u8string();
    std::string command = QuoteWindowsArgument(std::string(program_u8.begin(), program_u8.end()));
    for (const std::string& arg : args) command += " " + QuoteWindowsArgument(arg);
    std::wstring wide = Widen(command);

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &inherit, 0)) {
        result.error = "couldn't make a pipe (error " + std::to_string(GetLastError()) + ")";
        return result;
    }
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    HANDLE null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit,
                                 OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = null_in;
    startup.hStdOutput = write;
    startup.hStdError = write;
    PROCESS_INFORMATION process{};
    const BOOL ok = CreateProcessW(nullptr, wide.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                                   nullptr, nullptr, &startup, &process);
    const DWORD start_error = GetLastError();
    // the child has its own copies now
    CloseHandle(write);
    if (null_in != INVALID_HANDLE_VALUE) CloseHandle(null_in);
    if (!ok) {
        CloseHandle(read);
        result.error = start_error == ERROR_FILE_NOT_FOUND || start_error == ERROR_PATH_NOT_FOUND
                           ? "it isn't there"
                           : "it couldn't be started (error " + std::to_string(start_error) + ")";
        return result;
    }
    result.started = true;
    CloseHandle(process.hThread);
    char buffer[4096];
    for (;;) {
        if (cancel && cancel->load() && !result.cancelled) {
            TerminateProcess(process.hProcess, 1);
            result.cancelled = true;
        }
        DWORD available = 0;
        if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr)) break;  // closed: it's done
        if (available == 0) {
            // ended, with all it wrote read (a program it started may still
            // hold the pipe open)
            if (WaitForSingleObject(process.hProcess, 0) == WAIT_OBJECT_0) {
                if (!PeekNamedPipe(read, nullptr, 0, nullptr, &available, nullptr) || available == 0)
                    break;
                continue;
            }
            Sleep(20);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(read, buffer, std::min<DWORD>(available, sizeof(buffer)), &got, nullptr) ||
            got == 0)
            break;
        lines.Add(buffer, got);
    }
    lines.End();
    CloseHandle(read);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    result.exit_code = int(code);
    return result;
#else
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        result.error = std::string("couldn't make a pipe: ") + std::strerror(errno);
        return result;
    }
    // told apart from the program's own exit codes: exec failed
    int exec_error[2];
    if (pipe(exec_error) != 0) {
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        result.error = std::string("couldn't make a pipe: ") + std::strerror(errno);
        return result;
    }
    fcntl(exec_error[1], F_SETFD, FD_CLOEXEC);
    const std::string path = program.string();
    std::vector<std::string> argv_text{path};
    argv_text.insert(argv_text.end(), args.begin(), args.end());
    std::vector<char*> argv;
    for (std::string& a : argv_text) argv.push_back(a.data());
    argv.push_back(nullptr);
    const pid_t child = fork();
    if (child < 0) {
        result.error = std::string("couldn't start it: ") + std::strerror(errno);
        return result;
    }
    if (child == 0) {
        const int null_in = open("/dev/null", O_RDONLY);
        if (null_in >= 0) dup2(null_in, 0);
        dup2(pipe_fds[1], 1);
        dup2(pipe_fds[1], 2);
        close(pipe_fds[0]);
        close(exec_error[0]);
        // a name without a slash is looked for on PATH
        execvp(argv[0], argv.data());
        const int e = errno;
        (void)!write(exec_error[1], &e, sizeof(e));
        _exit(127);
    }
    close(pipe_fds[1]);
    close(exec_error[1]);
    int e = 0;
    if (read(exec_error[0], &e, sizeof(e)) == sizeof(e)) {
        close(exec_error[0]);
        close(pipe_fds[0]);
        waitpid(child, nullptr, 0);
        result.error = e == ENOENT ? "it isn't there" : std::string("it couldn't be started: ") + std::strerror(e);
        return result;
    }
    close(exec_error[0]);
    result.started = true;
    char buffer[4096];
    int status = 0;
    bool reaped = false;
    for (;;) {
        if (cancel && cancel->load() && !result.cancelled) {
            kill(child, SIGTERM);
            result.cancelled = true;
        }
        pollfd p{pipe_fds[0], POLLIN, 0};
        const int ready = poll(&p, 1, 50);
        if (ready < 0 && errno != EINTR) break;
        if (ready == 0) {
            // ended, with all it wrote read (a program it started may still
            // hold the pipe open)
            if (waitpid(child, &status, WNOHANG) == child) {
                reaped = true;
                break;
            }
            continue;
        }
        if (ready < 0) continue;
        const ssize_t got = read(pipe_fds[0], buffer, sizeof(buffer));
        if (got <= 0) break;
        lines.Add(buffer, size_t(got));
    }
    lines.End();
    close(pipe_fds[0]);
    if (!reaped) waitpid(child, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
#endif
}

}
