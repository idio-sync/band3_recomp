#include "src/Video/video_jobs.h"

#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "src/Render/native_view.h"
#include "src/Video/song_audio.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_decoder.h"
#include "src/Video/video_files.h"
#include "src/settings.h"

namespace band3::video {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kStatusFile = "band3_music_videos.json";

// leaked, so a job still running when band3 closes never touches a destroyed object
struct Jobs {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<VideoJob> waiting;
    bool started = false;
    bool paused = false;
    bool running = false;
    bool waiting_for_song = false;
    std::atomic<bool> cancel{false};
    std::string song;
    FetchStage stage = FetchStage::kSearching;
    double share = 0.0;
    size_t done = 0, total = 0, synced = 0, guessed = 0, review = 0, none = 0, failed = 0;
    SongVideoStatuses statuses;
    bool loaded = false;
    YtDlpState ytdlp;
    bool ytdlp_checked = false;
};

Jobs& TheJobs() {
    static Jobs* jobs = new Jobs;
    return *jobs;
}

fs::path StatusPath() {
    auto* runtime = rex::Runtime::instance();
    return runtime ? runtime->user_data_root() / kStatusFile : fs::path();
}

// the mutex held
void LoadLocked(Jobs& j) {
    if (j.loaded) return;
    j.loaded = true;
    std::ifstream in(StatusPath(), std::ios::binary);
    if (!in) return;
    std::stringstream text;
    text << in.rdbuf();
    j.statuses = ParseVideoStatuses(text.str());
}

void Save(const std::string& text) {
    const fs::path path = StatusPath();
    if (path.empty()) return;
    fs::path temp = path;
    temp += ".partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << text;
        if (!out) {
            REXLOG_WARN("Music videos: couldn't write {}", rex::path_to_utf8(temp));
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) REXLOG_WARN("Music videos: couldn't write {}: {}", rex::path_to_utf8(path), ec.message());
}

YtDlp MakeYtDlp() {
    const std::string path = REXCVAR_GET(music_video_ytdlp_path);
    return YtDlp(path.empty() ? fs::path() : rex::to_path(path),
                 SplitArgs(REXCVAR_GET(music_video_ytdlp_args)));
}

// the song's sound: one taken as it played beside its video, else its package's
bool SongEnvelope(const fs::path& folder, const SongInfo& song, std::vector<float>& env,
                  std::string& why) {
    if (auto video = FindVideo({folder}, song.shortname)) {
        if (auto kept = LoadEnvelope(SongEnvelopePath(video->path), 0)) {
            env = std::move(*kept);
            return true;
        }
    }
    std::string error;
    switch (PackageSongEnvelope(song.shortname, env, error)) {
        case SongAudioResult::kOk: return true;
        case SongAudioResult::kEncrypted: why = "its song's audio is encrypted"; break;
        case SongAudioResult::kNotFound: why = "its song isn't in band3's packages"; break;
        case SongAudioResult::kFailed: why = "its song's audio couldn't be read: " + error; break;
    }
    return false;
}

void Count(Jobs& j, FetchEnd end) {
    switch (end) {
        case FetchEnd::kDone:
        case FetchEnd::kUnsynced: j.synced++; break;
        case FetchEnd::kGuessed: j.guessed++; break;
        case FetchEnd::kReview: j.review++; break;
        case FetchEnd::kNoVideo: j.none++; break;
        case FetchEnd::kFailed: j.failed++; break;
        case FetchEnd::kStopped: break;
    }
}

void Run() {
    auto& j = TheJobs();
    // the still check opens videos on this thread
    std::string video_error;
    if (!StartVideoThread(video_error))
        REXLOG_WARN("Music videos: no video decoder ({}): downloads can't be checked", video_error);
    for (;;) {
        VideoJob job;
        {
            std::unique_lock lock(j.mutex);
            for (;;) {
                j.cv.wait(lock, [&] { return !j.waiting.empty() && !j.paused; });
                // a song being played: the menus are soon enough
                j.waiting_for_song = render::InSong();
                if (!j.waiting_for_song) break;
                j.cv.wait_for(lock, std::chrono::seconds(1));
            }
            job = std::move(j.waiting.front());
            j.waiting.pop_front();
            j.running = true;
            j.song = job.song.shortname;
            j.stage = FetchStage::kSearching;
            j.share = 0.0;
            j.cancel = false;
        }
        const YtDlp yt = MakeYtDlp();
        FetchSetup setup;
        setup.ytdlp = &yt;
        setup.folder = job.folder;
        setup.format = {REXCVAR_GET(music_video_max_height), REXCVAR_GET(music_video_prefer_60fps)};
        setup.auto_accept = REXCVAR_GET(music_video_auto_accept);
        setup.keep_audio = REXCVAR_GET(music_video_keep_audio);
        setup.song_envelope = [folder = job.folder](const SongInfo& s, std::vector<float>& env,
                                                    std::string& why) {
            return SongEnvelope(folder, s, env, why);
        };
        setup.progress = [&j](FetchStage stage, double share) {
            std::lock_guard lock(j.mutex);
            j.stage = stage;
            j.share = share;
        };
        setup.cancel = &j.cancel;
        FetchResult r;
        switch (job.kind) {
            case VideoJob::Kind::kFind: r = FindSongVideo(job.song, setup); break;
            case VideoJob::Kind::kLink: r = FetchSongVideo(job.song, job.link, setup); break;
            case VideoJob::Kind::kFile: r = UseVideoFile(job.song, job.file, setup); break;
        }
        REXLOG_INFO("Music videos: {}: {}{}{}", job.song.shortname, FetchEndText(r.end),
                    r.message.empty() ? "" : ": ", r.message);
        std::string text;
        {
            std::lock_guard lock(j.mutex);
            j.running = false;
            j.song.clear();
            j.done++;
            Count(j, r.end);
            LoadLocked(j);
            if (r.end != FetchEnd::kStopped) {
                SongVideoStatus& status = j.statuses[job.song.shortname];
                // a link chosen from a review keeps the review's list, in case it's wanted again
                if (!r.candidates.empty() || r.end != FetchEnd::kFailed) status.candidates = std::move(r.candidates);
                status.end = r.end;
                status.message = std::move(r.message);
                status.when = std::chrono::duration_cast<std::chrono::seconds>(
                                  std::chrono::system_clock::now().time_since_epoch())
                                  .count();
                text = FormatVideoStatuses(j.statuses);
            }
        }
        if (!text.empty()) Save(text);
    }
}

}

size_t QueueVideoJobs(std::vector<VideoJob> jobs) {
    // a song chosen by hand isn't searched for again (looked at outside the lock)
    std::erase_if(jobs, [](const VideoJob& job) {
        if (job.kind != VideoJob::Kind::kFind) return false;
        const auto video = FindVideo({job.folder}, job.song.shortname);
        return video && video->pinned;
    });
    auto& j = TheJobs();
    size_t added = 0;
    {
        std::lock_guard lock(j.mutex);
        if (!j.running && j.waiting.empty()) {
            j.done = j.total = j.synced = j.guessed = j.review = j.none = j.failed = 0;
        }
        for (VideoJob& job : jobs) {
            auto same = std::find_if(j.waiting.begin(), j.waiting.end(), [&](const VideoJob& w) {
                return w.song.shortname == job.song.shortname;
            });
            if (same != j.waiting.end()) {
                if (job.kind == VideoJob::Kind::kFind) continue;
                j.waiting.erase(same);
                j.total--;
            }
            j.waiting.push_back(std::move(job));
            j.total++;
            added++;
        }
        if (!j.started && added) {
            j.started = true;
            std::thread(Run).detach();
        }
    }
    j.cv.notify_all();
    return added;
}

void PauseVideoJobs(bool paused) {
    auto& j = TheJobs();
    {
        std::lock_guard lock(j.mutex);
        j.paused = paused;
    }
    j.cv.notify_all();
}

void StopVideoJobs() {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    j.total -= std::min(j.total, j.waiting.size());
    j.waiting.clear();
    j.cancel = true;
}

VideoJobsProgress VideoJobsNow() {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    VideoJobsProgress p;
    p.running = j.running;
    p.paused = j.paused;
    p.waiting_for_song = j.waiting_for_song && !j.waiting.empty();
    p.waiting = j.waiting.size();
    p.done = j.done;
    p.total = j.total;
    p.synced = j.synced;
    p.guessed = j.guessed;
    p.review = j.review;
    p.none = j.none;
    p.failed = j.failed;
    p.song = j.song;
    p.stage = j.stage;
    p.share = j.share;
    return p;
}

std::optional<SongVideoStatus> SongVideoStatusOf(std::string_view shortname) {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    LoadLocked(j);
    const auto it = j.statuses.find(shortname);
    if (it == j.statuses.end()) return std::nullopt;
    return it->second;
}

std::optional<FetchEnd> SongVideoEndOf(std::string_view shortname) {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    LoadLocked(j);
    const auto it = j.statuses.find(shortname);
    if (it == j.statuses.end()) return std::nullopt;
    return it->second.end;
}

bool SongVideoQueued(std::string_view shortname) {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    if (j.running && j.song == shortname) return true;
    return std::any_of(j.waiting.begin(), j.waiting.end(),
                       [&](const VideoJob& w) { return w.song.shortname == shortname; });
}

void ForgetSongVideoStatus(std::string_view shortname) {
    auto& j = TheJobs();
    std::string text;
    {
        std::lock_guard lock(j.mutex);
        LoadLocked(j);
        const auto it = j.statuses.find(shortname);
        if (it == j.statuses.end()) return;
        j.statuses.erase(it);
        text = FormatVideoStatuses(j.statuses);
    }
    Save(text);
}

YtDlpState YtDlpNow(bool again) {
    auto& j = TheJobs();
    std::lock_guard lock(j.mutex);
    if ((!j.ytdlp_checked || again) && !j.ytdlp.checking && !j.ytdlp.updating) {
        j.ytdlp_checked = true;
        j.ytdlp.checking = true;
        std::thread([&j] {
            std::string error;
            const std::optional<std::string> version = MakeYtDlp().Version(error);
            std::lock_guard lock(j.mutex);
            j.ytdlp.checking = false;
            j.ytdlp.version = version.value_or("");
            j.ytdlp.problem = version ? "" : error;
        }).detach();
    }
    return j.ytdlp;
}

void UpdateYtDlp() {
    auto& j = TheJobs();
    {
        std::lock_guard lock(j.mutex);
        if (j.ytdlp.updating || j.ytdlp.checking) return;
        j.ytdlp.updating = true;
    }
    std::thread([&j] {
        const YtDlp yt = MakeYtDlp();
        std::string output;
        yt.Update(output);
        std::string error;
        const std::optional<std::string> version = yt.Version(error);
        REXLOG_INFO("Music videos: yt-dlp -U: {}", output);
        std::lock_guard lock(j.mutex);
        j.ytdlp.updating = false;
        j.ytdlp.update_output = output;
        j.ytdlp.version = version.value_or("");
        j.ytdlp.problem = version ? "" : error;
    }).detach();
}

std::string_view FetchStageText(FetchStage stage) {
    switch (stage) {
        case FetchStage::kSearching: return "Searching";
        case FetchStage::kFetchingSound: return "Fetching its sound";
        case FetchStage::kMatching: return "Matching it with the song";
        case FetchStage::kFetchingVideo: return "Fetching the video";
        case FetchStage::kChecking: return "Checking its picture";
        case FetchStage::kSaving: return "Saving";
    }
    return "";
}

}
