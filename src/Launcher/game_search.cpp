#include "game_search.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <string>
#include <string_view>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace band3::launcher {

namespace fs = std::filesystem;

namespace {

std::string Lower(std::string text) {
    for (char& c : text) c = char(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

// folders no game is copied into, and hidden ones
bool PassedOver(const fs::path& folder) {
    const std::string name = Lower(folder.filename().string());
    if (name.empty() || name[0] == '.' || name[0] == '$') return true;
    constexpr std::string_view kSystem[] = {
        "windows", "system volume information", "programdata", "appdata", "node_modules",
        "winsxs", "proc", "sys", "dev", "snap", "recovery", "msocache",
    };
    return std::ranges::find(kSystem, name) != std::end(kSystem);
}

}

bool HoldsGame(const fs::path& folder) {
    std::error_code ec;
    return fs::is_regular_file(folder / "gen" / "main_xbox.hdr", ec);
}

std::vector<fs::path> ListFolders(const fs::path& folder) {
    std::vector<fs::path> out;
    std::error_code ec;
    fs::directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::directory_iterator(); it.increment(ec)) {
        std::error_code type_ec;
        if (!it->is_directory(type_ec) || PassedOver(it->path())) continue;
        out.push_back(it->path());
    }
    std::ranges::sort(out, [](const fs::path& a, const fs::path& b) {
        return Lower(a.filename().string()) < Lower(b.filename().string());
    });
    return out;
}

std::vector<fs::path> FindGameFolders(std::span<const fs::path> roots, const SearchLimits& limits,
                                      const std::atomic<bool>* stop, std::atomic<size_t>* looked_in) {
    const auto deadline = std::chrono::steady_clock::now() + limits.time;
    std::vector<fs::path> found;
    struct Pending {
        fs::path folder;
        int depth;
    };
    std::deque<Pending> queue;
    for (const fs::path& root : roots) queue.push_back({root, 0});
    size_t opened = 0;
    while (!queue.empty()) {
        if ((stop && stop->load()) || std::chrono::steady_clock::now() > deadline) break;
        const Pending next = std::move(queue.front());
        queue.pop_front();
        if (looked_in) ++*looked_in;
        if (HoldsGame(next.folder)) {
            found.push_back(next.folder);
            // the game's own folders hold no other copy
            continue;
        }
        if (next.depth >= limits.depth) continue;
        if (++opened > limits.max_folders) break;
        for (fs::path& child : ListFolders(next.folder)) queue.push_back({std::move(child), next.depth + 1});
    }
    return found;
}

std::vector<fs::path> SearchRoots() {
    std::vector<fs::path> roots;
#ifdef _WIN32
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; i++) {
        if (!(drives & (1u << i))) continue;
        const std::string root = std::string(1, char('A' + i)) + ":\\";
        const UINT type = GetDriveTypeA(root.c_str());
        // the PC's own drives and USB ones, not network shares or optical drives
        if (type == DRIVE_FIXED || type == DRIVE_REMOVABLE) roots.emplace_back(root);
    }
#else
    if (const char* home = std::getenv("HOME")) roots.emplace_back(home);
    for (const char* mounts : {"/media", "/run/media", "/mnt"}) {
        std::error_code ec;
        if (fs::is_directory(mounts, ec)) roots.emplace_back(mounts);
    }
#endif
    return roots;
}

std::string DriveText(std::string_view root, std::string_view label, uint64_t bytes) {
    std::string text(root);
    if (!label.empty()) text += "  " + std::string(label);
    if (bytes == 0) return text;
    char size[32];
    const double tb = double(bytes) / 1e12, gb = double(bytes) / 1e9, mb = double(bytes) / 1e6;
    if (tb >= 10) std::snprintf(size, sizeof(size), "%.0f TB", tb);
    else if (tb >= 1) std::snprintf(size, sizeof(size), "%.1f TB", tb);
    else if (gb >= 1) std::snprintf(size, sizeof(size), "%.0f GB", gb);
    else std::snprintf(size, sizeof(size), "%.0f MB", mb);
    return text + (label.empty() ? "  " : " \xC2\xB7 ") + size;
}

std::string DescribeRoot(const fs::path& root) {
    std::string label;
    uint64_t bytes = 0;
#ifdef _WIN32
    // an empty card reader answers at once, without asking for a disk
    DWORD old_mode = 0;
    SetThreadErrorMode(SEM_FAILCRITICALERRORS, &old_mode);
    wchar_t name[MAX_PATH + 1] = {};
    if (GetVolumeInformationW(root.c_str(), name, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0)) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, name, -1, nullptr, 0, nullptr, nullptr);
        if (n > 1) {
            label.resize(size_t(n - 1));
            WideCharToMultiByte(CP_UTF8, 0, name, -1, label.data(), n, nullptr, nullptr);
        }
        ULARGE_INTEGER total{};
        if (GetDiskFreeSpaceExW(root.c_str(), nullptr, &total, nullptr)) bytes = total.QuadPart;
    }
    SetThreadErrorMode(old_mode, nullptr);
#else
    std::error_code ec;
    const fs::space_info space = fs::space(root, ec);
    if (!ec) bytes = space.capacity;
#endif
    return DriveText(root.string(), label, bytes);
}

}
