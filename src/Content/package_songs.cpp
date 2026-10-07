#include "package_songs.h"

#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/xtypes.h>
#include "live_content.h"
#include "package_scan.h"

namespace band3::content {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kCacheFile = "band3_package_songs.json";
// bigger than any pack's songs.dta
constexpr size_t kMaxDta = size_t{16} << 20;

// leaked, so a read still going when band3 closes never touches a destroyed object
struct State {
    std::mutex mutex;
    std::vector<PackageSongs> packages;  // the last read in full
    std::vector<fs::path> listed;        // the packages it was of (or is, while reading)
    bool reading = false;
    size_t read = 0;
    size_t total = 0;
};

State& TheState() {
    static State* state = new State;
    return *state;
}

fs::path CachePath() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->user_data_root() / kCacheFile : fs::path();
}

// the package's songs/songs.dta, opened as the game opens packages; nullopt
// when there's none, or it can't be read
std::optional<std::string> ReadSongsDta(const fs::path& path) {
    rex::filesystem::StfsContainerDevice device("\\Device\\Band3PackageSongs\\", path);
    if (!device.Initialize()) return std::nullopt;
    rex::filesystem::Entry* entry = device.ResolvePath("songs\\songs.dta");
    if (!entry || entry->size() > kMaxDta) return std::nullopt;
    rex::filesystem::File* file = nullptr;
    if (!XSUCCEEDED(entry->Open(rex::filesystem::FileAccess::kGenericRead, &file)) || !file) {
        return std::nullopt;
    }
    std::string text(entry->size(), '\0');
    size_t got = 0;
    const rex::X_STATUS status =
        file->ReadSync(std::span(reinterpret_cast<uint8_t*>(text.data()), text.size()), 0, &got);
    file->Destroy();
    if (!XSUCCEEDED(status)) return std::nullopt;
    text.resize(got);
    return text;
}

void WriteCache(const std::vector<PackageSongs>& packages) {
    const fs::path path = CachePath();
    if (path.empty()) return;
    fs::path temp = path;
    temp += kPartialSuffix;
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file << FormatInventoryCache(packages);
        if (!file) {
            REXLOG_WARN("Package songs: couldn't write {}", rex::path_to_utf8(temp));
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) REXLOG_WARN("Package songs: couldn't write {}: {}", rex::path_to_utf8(path), ec.message());
}

void ReadAll(std::vector<fs::path> paths) {
    std::unordered_map<std::string, PackageSongs> cache;
    if (std::ifstream file(CachePath(), std::ios::binary); file) {
        std::ostringstream text;
        text << file.rdbuf();
        for (PackageSongs& package : ParseInventoryCache(text.str())) {
            std::string key = package.path;
            cache.emplace(std::move(key), std::move(package));
        }
    }
    auto& state = TheState();
    std::vector<PackageSongs> packages;
    size_t opened = 0;
    for (const fs::path& path : paths) {
        PackageSongs package;
        package.path = rex::path_to_utf8(path);
        std::error_code size_ec, time_ec;
        package.size = static_cast<int64_t>(fs::file_size(path, size_ec));
        package.modified = fs::last_write_time(path, time_ec).time_since_epoch().count();
        const auto cached = cache.find(package.path);
        if (!size_ec && !time_ec && cached != cache.end() && cached->second.size == package.size &&
            cached->second.modified == package.modified) {
            package = cached->second;
        } else {
            opened++;
            if (const auto text = ReadSongsDta(path)) {
                package.songs = ParseSongsDta(*text);
            } else {
                package.unreadable = true;
                REXLOG_WARN("Package songs: couldn't read songs.dta in {}", package.path);
            }
        }
        packages.push_back(std::move(package));
        std::lock_guard lock(state.mutex);
        state.read++;
    }
    // what's gone from the folders goes from the cache too
    if (opened || packages.size() != cache.size()) WriteCache(packages);
    REXLOG_INFO("Package songs: {} packages, {} of them read now", packages.size(), opened);
    std::lock_guard lock(state.mutex);
    state.packages = std::move(packages);
    state.reading = false;
}

}

PackageSongsSnapshot CurrentPackageSongs() {
    // outside the lock: it waits for the content scan at first
    std::vector<fs::path> paths;
    for (const Package& package : LivePackages()) paths.push_back(package.path);
    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    if (!state.reading && paths != state.listed) {
        state.listed = paths;
        state.reading = true;
        state.read = 0;
        state.total = paths.size();
        std::thread(ReadAll, std::move(paths)).detach();
    }
    PackageSongsSnapshot snapshot;
    snapshot.packages = state.packages;
    snapshot.status = {state.reading, state.read, state.total, false};
    return snapshot;
}

}
