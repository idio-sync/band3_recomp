#pragma once
#include <set>
#include <string>
#include <string_view>
#include <vector>

// On Linux the SDK backs the guest's memory with a shared memory file,
// /dev/shm/xenia_memory_<n>, 4.5 GiB long, and removes it only when its
// Memory is destroyed. band3 ends without that (closing the window ends the
// process without OnShutdown; a crash or a kill never gets there), so each
// run left its file behind, holding every page the game touched in RAM: a
// Steam Deck's /dev/shm (half its RAM) filled after a handful of runs, the
// rest went to swap, and the next start died of SIGBUS touching its new
// file. The mapping doesn't need the name once it's made, so band3 unlinks
// its own at startup, and with it the files earlier runs left that no
// process maps any more.

namespace band3::guest_memory_file {

inline constexpr std::string_view kShmDir = "/dev/shm/";
inline constexpr std::string_view kPrefix = "xenia_memory_";

// whether `name` (a /dev/shm entry's) is a guest memory file's
inline bool IsGuestMemoryName(std::string_view name) {
    return name.size() > kPrefix.size() && name.substr(0, kPrefix.size()) == kPrefix;
}

// The guest memory files a /proc/<pid>/maps text maps, as /dev/shm paths.
// A line's path is its sixth field on: "start-end perms offset dev inode
// path", " (deleted)" after it once unlinked (still mapped, but no file to
// remove).
inline std::set<std::string> MappedGuestMemoryFiles(std::string_view maps) {
    std::set<std::string> out;
    size_t at = 0;
    while (at < maps.size()) {
        size_t end = maps.find('\n', at);
        if (end == std::string_view::npos) end = maps.size();
        const std::string_view line = maps.substr(at, end - at);
        at = end + 1;
        const size_t path = line.find(kShmDir);
        if (path == std::string_view::npos) continue;
        std::string_view p = line.substr(path);
        if (p.ends_with(" (deleted)")) continue;
        if (!IsGuestMemoryName(p.substr(kShmDir.size()))) continue;
        out.emplace(p);
    }
    return out;
}

// Of `files` (guest memory files in /dev/shm), those to remove: this
// process's own (`own`), and those no process maps (`mapped`, every
// readable process's, this one's included)
inline std::vector<std::string> FilesToRemove(const std::vector<std::string>& files,
                                              const std::set<std::string>& own,
                                              const std::set<std::string>& mapped) {
    std::vector<std::string> out;
    for (const std::string& f : files)
        if (own.contains(f) || !mapped.contains(f)) out.push_back(f);
    return out;
}

// Unlinks this process's guest memory file and the ones earlier runs left,
// logging what it freed. Once the SDK's memory is mapped (OnPostSetup).
// Nothing off Linux.
void Release();

}  // namespace band3::guest_memory_file
