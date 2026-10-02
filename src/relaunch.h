#pragma once
#include <string>
#include <string_view>
#include <vector>

// Restarting band3, for RB3Enhanced's rb3e_relaunch_game (Rock Band 3 Deluxe
// relaunches after clearing the song cache and after some settings).

namespace band3::relaunch {

// Starts band3 again with this run's command line, less any --launcher, plus
// extra_args (each one argument, e.g. "--launcher=false"), and its window
// shown the same way (a minimized test run stays minimized). The new one waits
// for this process to exit before it starts, so the caller ends this one next.
// False when it couldn't be started.
bool StartAgain(const std::vector<std::string>& extra_args = {});

// With relaunch_wait_pid set (StartAgain sets it), waits up to 30 s for that
// process to exit, then clears the setting so "Save to config" can't keep it.
// Call before anything opens files or ports.
void WaitForPrevious();

// Whether this run was started by StartAgain, as WaitForPrevious found; the
// launcher doesn't show for a relaunch.
bool WasRelaunched();

// What StartAgain drops from this run's arguments (pure, for the tests)

// Linux, one argv entry: "--name" or "--name=value" ("--name_more" is another
// setting)
inline bool IsFlag(std::string_view arg, std::string_view name) {
    if (!arg.starts_with("--") || arg.substr(2, name.size()) != name) return false;
    const std::string_view rest = arg.substr(2 + name.size());
    return rest.empty() || rest.front() == '=';
}

// Windows, a whole command line (GetCommandLineW): drops each " --name" and
// " --name=value" argument. " --name_more" is another setting and stays, and
// so does anything inside quotes, such as a folder with " --name" in it. A
// quote preceded by an odd number of backslashes is a literal one, as the
// command line's own parsing has it.
inline void EraseFlag(std::wstring& command, std::wstring_view name) {
    const std::wstring flag = L"--" + std::wstring(name);
    auto blank = [](wchar_t c) { return c == L' ' || c == L'\t'; };
    bool quoted = false;
    size_t backslashes = 0;
    // where the argument starting at `from` ends: the first blank outside quotes
    auto argument_end = [&](size_t from) {
        bool in_quotes = false;
        size_t slashes = 0;
        for (size_t i = from; i < command.size(); i++) {
            const wchar_t c = command[i];
            if (c == L'"' && slashes % 2 == 0) in_quotes = !in_quotes;
            slashes = c == L'\\' ? slashes + 1 : 0;
            if (!in_quotes && blank(c)) return i;
        }
        return command.size();
    };
    for (size_t i = 0; i < command.size();) {
        const wchar_t c = command[i];
        if (c == L'"' && backslashes % 2 == 0) quoted = !quoted;
        backslashes = c == L'\\' ? backslashes + 1 : 0;
        if (!quoted && blank(c) && command.compare(i + 1, flag.size(), flag) == 0) {
            const size_t after = i + 1 + flag.size();
            if (after == command.size() || blank(command[after]) || command[after] == L'=') {
                // the blank before it goes too; what's at i is then the next
                // blank, or the end
                command.erase(i, argument_end(after) - i);
                continue;
            }
        }
        i++;
    }
}

}
