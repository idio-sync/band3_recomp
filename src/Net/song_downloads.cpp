#include "song_downloads.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <deque>
#include <iterator>
#include <fstream>
#include <mutex>
#include <sstream>
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
// The update check at launch waits for the game's own loading first, and asks
// nothing when the last check was this recent (a launch after a launch);
// asked for on the page, it goes straight away
constexpr std::chrono::seconds kCheckDelay{30};
constexpr int64_t kCheckInterval = 12 * 60 * 60;
// between the pages it reads, as RhythmVerse's API isn't band3's to load
constexpr std::chrono::seconds kCheckPause{2};
// past this many, the check stops where it is: years of RhythmVerse's updates,
// which only a download folder older than band3's downloading would need
constexpr int32_t kMaxCheckPages = 50;

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
    // rhythmverse.json's, read when first needed (mutex held)
    DownloadRecords records;
    CheckState check;
    bool records_loaded = false;
    // the update check: its thread running, asking RhythmVerse, asked for on
    // the page, and why the last one failed
    bool check_thread = false;
    bool checking = false;
    bool check_asked = false;
    std::string check_error;
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

fs::path RecordsPath() { return DownloadFolder() / "rhythmverse.json"; }

int64_t NowSeconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

// rhythmverse.json as written (FormatRecords); the state's mutex not held,
// since the folder may be on a slow drive. Writes go one at a time.
void WriteRecords(const std::string& text) {
    static std::mutex write_mutex;
    std::lock_guard lock(write_mutex);
    const fs::path path = RecordsPath();
    fs::path temp = path;
    temp += content::kPartialSuffix;
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file << text;
        if (!file) {
            REXLOG_WARN("RhythmVerse: couldn't write {}", rex::path_to_utf8(temp));
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) REXLOG_WARN("RhythmVerse: couldn't write {}: {}", rex::path_to_utf8(path), ec.message());
}

// The records, read from rhythmverse.json the first time; the state's mutex
// not held. An update that was waiting is the version downloaded once it's
// in place (the content scan puts it there, maybe after the first read).
DownloadRecords CurrentRecords() {
    auto& state = TheState();
    const fs::path folder = DownloadFolder();
    if (folder.empty()) return {};
    bool loaded;
    {
        std::lock_guard lock(state.mutex);
        loaded = state.records_loaded;
    }
    if (!loaded) {
        std::string text;
        if (std::ifstream file(RecordsPath(), std::ios::binary); file) {
            std::ostringstream stream;
            stream << file.rdbuf();
            text = stream.str();
        }
        DownloadRecords read = ParseRecords(text);
        // a record from before band3 kept when it downloaded: its file's time
        bool filled = false;
        for (auto& [file_id, record] : read) {
            if (record.downloaded) continue;
            std::error_code ec;
            const auto time = fs::last_write_time(folder / rex::to_path(record.file_name), ec);
            if (ec) continue;
            record.downloaded = std::chrono::duration_cast<std::chrono::seconds>(
                                    std::chrono::clock_cast<std::chrono::system_clock>(time)
                                        .time_since_epoch())
                                    .count();
            filled = true;
        }
        std::string written;
        {
            std::lock_guard lock(state.mutex);
            if (!state.records_loaded) {
                // a download that finished meanwhile is newer than the file
                for (auto& [id, record] : state.records) read.insert_or_assign(id, record);
                state.records = std::move(read);
                state.check = ParseCheckState(text);
                state.records_loaded = true;
                if (filled) written = FormatRecords(state.records, state.check);
            }
        }
        if (!written.empty()) WriteRecords(written);
    }
    DownloadRecords records;
    {
        std::lock_guard lock(state.mutex);
        records = state.records;
    }
    std::vector<std::string> in_place;
    for (const auto& [file_id, record] : records) {
        if (record.pending_hash.empty()) continue;
        std::error_code ec;
        const fs::path file = folder / rex::to_path(record.file_name);
        fs::path pending = file;
        pending += content::kPendingSuffix;
        if (!fs::exists(pending, ec) && fs::exists(file, ec)) in_place.push_back(file_id);
    }
    if (in_place.empty()) return records;
    std::string text;
    {
        std::lock_guard lock(state.mutex);
        for (const auto& file_id : in_place) {
            auto it = state.records.find(file_id);
            if (it == state.records.end() || it->second.pending_hash.empty()) continue;
            it->second.hash = std::move(it->second.pending_hash);
            it->second.pending_hash.clear();
        }
        records = state.records;
        text = FormatRecords(state.records, state.check);
    }
    WriteRecords(text);
    return records;
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

// "" and the file it's in (name, in folder), or why not
std::string DownloadOne(const Song& song, const fs::path& folder, const fs::path& name,
                        fs::path& path) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) return "couldn't make the folder " + rex::path_to_utf8(folder) + ": " + ec.message();
    path = folder / name;
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
        // first, outside the lock: it may read the download folder
        const DownloadRecords records = CurrentRecords();
        Song song;
        bool update = false;
        // an update goes beside the file it's for, which the game may have
        // open, and takes its place at the next launch (content/package_scan.h)
        fs::path name;
        {
            std::lock_guard lock(state.mutex);
            if (state.queue.empty() || state.stopping) {
                state.working = false;
                return;
            }
            const std::string file_id = state.queue.front();
            state.queue.pop_front();
            song = state.known.at(file_id);
            if (Download* d = FindDownload(state, file_id)) {
                d->state = Download::State::kDownloading;
                update = d->update;
            }
            name = DownloadFileName(song);
            const auto record = records.find(file_id);
            if (update && record != records.end()) name = rex::to_path(record->second.file_name);
            if (update) name += content::kPendingSuffix;
        }
        REXLOG_INFO("RhythmVerse: {} {} - {} ({})", update ? "updating" : "downloading", song.artist,
                    song.title, song.file_id);
        fs::path path;
        const std::string error = DownloadOne(song, folder, name, path);
        if (error.empty()) {
            REXLOG_INFO("RhythmVerse: downloaded {} - {}", song.artist, song.title);
        } else {
            REXLOG_WARN("RhythmVerse: couldn't download {} - {}: {}", song.artist, song.title, error);
        }
        {
            std::lock_guard lock(state.files_mutex);
            state.files_stale = true;
        }
        std::string records_text;
        {
            std::lock_guard lock(state.mutex);
            if (error.empty()) {
                // what was downloaded, to tell when there's a newer version
                if (update) {
                    DownloadRecord& record = state.records[song.file_id];
                    record.pending_hash = song.hash;
                    record.latest = song;
                } else {
                    state.records[song.file_id] = {
                        .file_name = rex::path_to_utf8(path.filename()),
                        .hash = song.hash,
                        .pending_hash = {},
                        .downloaded = NowSeconds(),
                        .latest = song,
                    };
                }
                records_text = FormatRecords(state.records, state.check);
            }
            if (Download* d = FindDownload(state, song.file_id)) {
                d->state = error.empty() ? Download::State::kDone : Download::State::kFailed;
                d->error = error;
            }
        }
        if (!records_text.empty()) WriteRecords(records_text);
        // for the game to take it in (live_content.h); an update waits for the
        // next launch
        if (error.empty() && !update) content::AddLivePackages({path});
    }
}

// a while, or less once band3 is closing (false) or, with `until_asked`, once
// the page asks for a check
bool Wait(std::chrono::seconds time, bool until_asked) {
    auto& state = TheState();
    constexpr std::chrono::milliseconds kStep{200};
    for (std::chrono::milliseconds waited{0}; waited < time; waited += kStep) {
        if (state.stopping) return false;
        if (until_asked) {
            std::lock_guard lock(state.mutex);
            if (state.check_asked) return true;
        }
        std::this_thread::sleep_for(kStep);
    }
    return !state.stopping;
}

// the check's end: how far it read when it got to the end, why not when it
// failed, or neither when there was nothing to ask
void EndCheck(const std::optional<CheckState>& check, const std::string& error) {
    auto& state = TheState();
    std::string text;
    size_t updates = 0;
    {
        std::lock_guard lock(state.mutex);
        state.check_thread = false;
        state.checking = false;
        state.check_asked = false;
        state.check_error = error;
        if (check) {
            state.check = *check;
            text = FormatRecords(state.records, state.check);
        }
        for (const auto& [file_id, record] : state.records) {
            if (RecordUpdate(file_id, state.records) == UpdateState::kAvailable) updates++;
        }
    }
    if (!text.empty()) WriteRecords(text);
    if (!error.empty()) {
        REXLOG_WARN("RhythmVerse: couldn't check for updates: {}", error);
    } else if (check) {
        REXLOG_INFO("RhythmVerse: checked for updates, {} available", updates);
    }
}

// RhythmVerse's uploads, the most recently updated first, back to what the
// records need (FeedStop), noting the versions of those band3 downloaded
void CheckWork() {
    auto& state = TheState();
    bool asked;
    {
        std::lock_guard lock(state.mutex);
        asked = state.check_asked;
    }
    if (!asked && !Wait(kCheckDelay, true)) return EndCheck(std::nullopt, {});
    {
        std::lock_guard lock(state.mutex);
        asked = state.check_asked;
    }
    const DownloadRecords records = CurrentRecords();
    CheckState check;
    {
        std::lock_guard lock(state.mutex);
        check = state.check;
    }
    const auto stop = FeedStop(records, check);
    if (!stop || (!asked && check.at && NowSeconds() - check.at < kCheckInterval)) {
        return EndCheck(std::nullopt, {});
    }
    {
        std::lock_guard lock(state.mutex);
        state.checking = true;
        state.check_error.clear();
    }
    int64_t newest = check.through;
    for (int32_t page = 1;; page++) {
        if (page > 1 && !Wait(kCheckPause, false)) return EndCheck(std::nullopt, {});
        const SearchRequest request = UpdateFeed(page);
        const web::Reply reply = web::PostForm(request.url, request.form);
        if (!reply.error.empty()) return EndCheck(std::nullopt, "couldn't reach RhythmVerse: " + reply.error);
        if (reply.status != 200) {
            return EndCheck(std::nullopt, "RhythmVerse answered " + std::to_string(reply.status));
        }
        const auto result = ParseSearch(reply.body);
        if (!result) return EndCheck(std::nullopt, "RhythmVerse's reply wasn't one band3 can read");
        for (const Song& song : result->songs) newest = std::max(newest, song.updated);
        std::string text;
        {
            std::lock_guard lock(state.mutex);
            if (NoteLatest(state.records, result->songs)) text = FormatRecords(state.records, state.check);
        }
        if (!text.empty()) WriteRecords(text);
        if (FeedReachedBack(result->songs, *stop) || int64_t{page} * request.page_size >= result->total) {
            break;
        }
        if (page == kMaxCheckPages) {
            REXLOG_WARN("RhythmVerse: the update check stopped after {} pages", page);
            break;
        }
    }
    EndCheck(CheckState{newest, NowSeconds()}, {});
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

QueueResult QueueDownload(std::string_view file_id, bool update) {
    if (!ValidFileId(file_id)) return QueueResult::kUnknown;
    const fs::path folder = DownloadFolder();
    // listed before the lock: the folders may be on a slow drive
    LocalSongs local;
    local.files = LocalFiles();
    local.records = CurrentRecords();

    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    auto it = state.known.find(std::string(file_id));
    // an update goes by the latest version the record has seen: the update
    // check finds songs no search has
    if (update) {
        const auto record = local.records.find(std::string(file_id));
        if (record != local.records.end() && record->second.latest) {
            const auto [at, added] = state.known.insert_or_assign(record->first, *record->second.latest);
            if (added) state.known_order.push_back(record->first);
            it = at;
        }
    }
    if (it == state.known.end()) return QueueResult::kUnknown;
    const Song& song = it->second;
    if (song.download_url.empty()) return QueueResult::kNotHosted;
    if (folder.empty()) return QueueResult::kNoFolder;
    Download* d = FindDownload(state, file_id);
    if (d && (d->state == Download::State::kQueued || d->state == Download::State::kDownloading)) {
        return QueueResult::kQueued;
    }
    if (update) {
        if (UpdateOf(song, local.records) != UpdateState::kAvailable) return QueueResult::kNoUpdate;
    } else if (IsDownloaded(song, local)) {
        return QueueResult::kHave;
    }
    // a failed one is tried again
    if (!d) {
        state.downloads.push_back(Download{song.file_id, song.title, song.artist});
        d = &state.downloads.back();
        d->song_id = song.song_id;
    }
    d->update = update;
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

DownloadRecords Records() { return CurrentRecords(); }

void NoteSeen(const std::vector<Song>& songs) {
    // read first, so a record noted here isn't taken for one newer than the file's
    CurrentRecords();
    auto& state = TheState();
    std::string text;
    {
        std::lock_guard lock(state.mutex);
        if (NoteLatest(state.records, songs)) text = FormatRecords(state.records, state.check);
    }
    if (!text.empty()) WriteRecords(text);
}

void CheckForUpdates(bool at_launch) {
    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    if (!at_launch) state.check_asked = true;
    if (state.check_thread || state.stopping) return;
    state.check_thread = true;
    std::thread(CheckWork).detach();
}

UpdateCheckStatus CheckStatus() {
    auto& state = TheState();
    std::lock_guard lock(state.mutex);
    return {state.checking, state.check.at, state.check_error};
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
            if (content::IsSetAside(it->path())) continue;
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
