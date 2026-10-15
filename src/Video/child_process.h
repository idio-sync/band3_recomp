#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Runs another program (yt-dlp, ytdlp.h) and reads what it prints: its own
// arguments, no shell, no console window, its output and errors read line
// by line as they come, and stopped if asked to be.

namespace band3::video {

struct ProcessResult {
    bool started = false;    // false: it couldn't be run (`error` says why)
    bool cancelled = false;  // stopped because `cancel` was set
    int exit_code = -1;
    std::string error;
};

// Runs `program` with `args` and waits for it, handing each line it prints
// (standard output and errors together, without the line ending) to
// `on_line` on this thread. With `cancel` set, the program is ended.
ProcessResult RunProcess(const std::filesystem::path& program, const std::vector<std::string>& args,
                         const std::function<void(std::string_view)>& on_line,
                         const std::atomic<bool>* cancel = nullptr);

// LD_PRELOAD's value without Steam's overlay (gameoverlayrenderer.so), which
// Steam preloads into a game it starts and so into each program the game
// runs: in its 32-bit and 64-bit builds, so the dynamic loader prints an
// error for the one that doesn't fit into a program's output (yt-dlp's
// version became "ERROR: ld.so: ..."). Entries are separated by colons or
// spaces, as ld.so takes them; for tests on every system.
std::string WithoutSteamOverlay(std::string_view ld_preload);

// One argument as a Windows command line quotes it (CommandLineToArgvW's
// rules), so any text arrives as it was; for tests on every system.
std::string QuoteWindowsArgument(std::string_view arg);

}
