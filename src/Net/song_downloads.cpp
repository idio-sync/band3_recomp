#include "song_downloads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iterator>
#include <mutex>
#include <span>
#include <thread>
#include <unordered_map>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include "src/Content/live_content.h"
#include "src/Content/package_scan.h"
#include "src/config.h"
#include "src/paths.h"
#include "src/settings.h"
#include "web_client.h"

namespace band3::rhythmverse {

namespace fs = std::filesystem;

namespace {

// songs found by searches, kept for downloading; past this many they're
// forgotten, and the page's next search finds them again
constexpr size_t kMaxRemembered = 5000;
// bigger than any song's package
constexpr int64_t kMaxBytes = int64_t{4} << 30;
// how long LocalFiles' listing is kept: songs copied into the folders show up
// as downloaded this long after
constexpr std::chrono::seconds kListingAge{60};

// leaked, so a download still running when band3 closes never touches a
// destroyed object
struct State {
    std::mutex mutex;
    std::unordered_map<std::string, Song> known;
    std::deque<std::string> known_order;  // oldest first, for forgetting
    std::vector<Download> downloads;
    std::deque<std::string> queue;
    bool working = false;  // the download thread is running
    std::atomic<bool> stopping{false};
    // LocalFiles' listing, and when it was made; listings happen one at a time
    std::mutex files_mutex;
    std::set<std::pair<std::string, int64_t>> files;
    std::set<fs::path> paths;  // the same files' paths
    std::chrono::steady_clock::time_point files_time;
    bool files_stale = true;
};

State& TheState() {
    static State* state = new State;
    return *state;
}

// the state's mutex held
Download* FindDownload(State& state, std::string_view file_id) {
    for (auto& d : state.downloads) {
        if (d.file_id == file_id) return &d;
    }
    return nullptr;
}

std::string CheckPackage(std::string_view first) {
    const auto header = content::ParsePackageHeader(
        std::span(reinterpret_cast<const uint8_t*>(first.data()), first.size()));
    if (!header) return "it isn't an Xbox 360 package (CON or LIVE), so it wasn't kept";
    if (std::ranges::find(content::kRb3TitleIds, header->title_id) == std::end(content::kRb3TitleIds)) {
        return "it isn't a Rock Band package, so it wasn't kept";
    }
    return {};
}

// downloads left half done by a band3 that didn't close normally
void RemovePartials(const fs::path& folder) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(folder, ec)) {
        if (entry.path().extension() == fs::path(content::kPartialSuffix)) {
            fs::remove(entry.path(), ec);
        }
    }
}

// "" and the file it's in, or why not
std::string DownloadOne(const Song& song, const fs::path& folder, fs::path& path) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return "couldn't make the folder " + rex::path_to_utf8(folder) + ": " + ec.message();
    path = folder / DownloadFileName(song);
    fs::path partial = path;
    partial += content::kPartialSuffix;

    auto& state = TheState();
    web::DownloadHooks hooks;
    hooks.peek = content::kHeaderBytes;
    hooks.check = CheckPackage;
    hooks.progress = [&](int64_t received, int64_t total) {
        std::lock_guard lock(state.mutex);
        if (Download* d = FindDownload(state, song.file_id)) {
            d->received = received;
            d->total = total;
        }
        return !state.stopping.load();
    };
    std::string error = web::Download(song.download_url, partial, hooks, kMaxBytes);
    if (!error.empty()) return error;
    fs::rename(partial, path, ec);
    if (ec) {
        fs::remove(partial, ec);
        return "couldn't put the file in place: " + ec.message();
    }
    return {};
}

void Work() {
    auto& state = TheState();
    const fs::path folder = DownloadFolder();
    RemovePartials(folder);
    while (true) {
        Song song;
        {
            std::lock_guard lock(state.mutex);
            if (state.queue.empty() || state.stopping) {
                state.working = false;
                return;
            }
            const std::string file_id = state.queue.front();
            state.queue.pop_front();
            song = state.known.at(file_id);
            if (Download* d = FindDownload(state, file_id)) d->state = Download::State::kDownloading;
        }
        REXLOG_INFO("RhythmVerse: downloading {} - {} ({})", song.artist, song.title, song.file_id);
        fs::path path;
        const std::string error = DownloadOne(song, folder, path);
        if (error.empty()) {
            REXLOG_INFO("RhythmVerse: downloaded {} - {}", song.artist, song.title);
        } else {
            REXLOG_WARN("RhythmVerse: couldn't download {} - {}: {}", song.artist, song.title, error);
        }
        {
            std::lock_guard lock(state.files_mutex);
            state.files_stale = true;
        }
        {
            std::lock_guard lock(state.mutex);
            if (Download* d = FindDownload(state, song.file_id)) {
                d->state = error.empty() ? Download::State::kDone : Download::State::kFailed;
                d->error = error;
            }
        }
        // for the game to take it in (live_content.h)
        if (error.empty()) content::AddLivePackages({path});
    }
}

}

fs::path DownloadFolder() {
    const auto folders = paths::SplitList(REXCVAR_GET(content_folders));
    if (folders.empty()) return {};
    return paths::Resolve(folders.front(), IniAnchor()) / "rhythmverse";
}

void Remember(const std::vector<Song>& songs) {
    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    for (const Song& song : songs) {
        if (state.known.insert_or_assign(song.file_id, song).second) {
            state.known_order.push_back(song.file_id);
        }
    }
    // the oldest go, so the page's latest results stay downloadable; queued
    // ones stay, since the download thread looks them up
    for (size_t tries = state.known_order.size();
         state.known.size() > kMaxRemembered && tries > 0; tries--) {
        std::string id = std::move(state.known_order.front());
        state.known_order.pop_front();
        if (std::ranges::find(state.queue, id) != state.queue.end()) {
            state.known_order.push_back(std::move(id));
            continue;
        }
        state.known.erase(id);
    }
}

QueueResult QueueDownload(std::string_view file_id) {
    if (!ValidFileId(file_id)) return QueueResult::kUnknown;
    const fs::path folder = DownloadFolder();
    // listed before the lock: the folders may be on a slow drive
    const LocalSongs local{LocalFiles(), std::nullopt};

    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    const auto it = state.known.find(std::string(file_id));
    if (it == state.known.end()) return QueueResult::kUnknown;
    const Song& song = it->second;
    if (song.download_url.empty()) return QueueResult::kNotHosted;
    if (folder.empty()) return QueueResult::kNoFolder;
    Download* d = FindDownload(state, file_id);
    if (d && (d->state == Download::State::kQueued || d->state == Download::State::kDownloading)) {
        return QueueResult::kQueued;
    }
    if (IsDownloaded(song, local)) return QueueResult::kHave;
    // a failed one is tried again
    if (!d) {
        state.downloads.push_back(Download{song.file_id, song.title, song.artist});
        d = &state.downloads.back();
        d->song_id = song.song_id;
    }
    d->state = Download::State::kQueued;
    d->received = 0;
    d->total = song.size;
    d->error.clear();
    state.queue.push_back(song.file_id);
    if (!state.working && !state.stopping) {
        state.working = true;
        std::thread(Work).detach();
    }
    return QueueResult::kQueued;
}

std::vector<Download> Downloads() {
    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    return state.downloads;
}

std::set<std::pair<std::string, int64_t>> LocalFiles() {
    auto& state = TheState();
    std::lock_guard lock(state.files_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (!state.files_stale && now - state.files_time < kListingAge) return state.files;

    std::set<std::pair<std::string, int64_t>> files;
    std::set<fs::path> paths;
    for (const auto& entry : paths::SplitList(REXCVAR_GET(content_folders))) {
        std::error_code ec;
        // as the content scan does: symlinks and junctions aren't followed
        fs::recursive_directory_iterator it(paths::Resolve(entry, IniAnchor()),
                                            fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) continue;
            if (it->path().extension() == fs::path(content::kPartialSuffix)) continue;
            const uintmax_t size = it->file_size(file_ec);
            if (file_ec) continue;
            files.emplace(LowerAscii(rex::path_to_utf8(it->path().filename())),
                          static_cast<int64_t>(size));
            paths.insert(it->path());
        }
    }
    // files copied in by hand (or, at the first listing, since the content
    // scan): the game takes in packages among them (live_content.h), which
    // reads only the ones it doesn't know
    std::vector<fs::path> added;
    std::ranges::set_difference(paths, state.paths, std::back_inserter(added));
    if (!added.empty()) {
        std::thread([added = std::move(added)] { content::AddLivePackages(added); }).detach();
    }
    state.paths = std::move(paths);
    state.files = std::move(files);
    state.files_time = now;
    state.files_stale = false;
    return state.files;
}

void StopDownloads() {
    TheState().stopping = true;
    content::StopLiveContent();
}

}
