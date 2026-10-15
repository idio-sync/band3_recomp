#include "video_library.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <rex/filesystem.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include "src/Content/game_song_list.h"
#include "src/Content/package_songs.h"
#include "src/Video/auto_sync.h"
#include "src/Video/music_video.h"
#include "src/Video/sync_cache.h"
#include "src/Video/video_files.h"

namespace band3::launcher {

namespace fs = std::filesystem;
using video::FetchEnd;

struct VideoLibrary::SyncJob {
    std::mutex mutex;
    fs::path video;
    bool done = false;
    std::optional<video::SyncResult> result;
    std::string error;
};

struct VideoLibrary::FilePick {
    std::mutex mutex;
    bool done = false;
    Song song;
    std::optional<std::string> file;
    std::string error;
};

namespace {

// how often the folder is looked at again while the screen shows, and the songs
constexpr auto kVideosEvery = std::chrono::seconds(2);
constexpr auto kSongsEvery = std::chrono::seconds(10);

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool IsVideo(const fs::path& p) {
    const std::string ext = Lower(p.extension().string());
    return std::any_of(std::begin(video::kVideoExtensions), std::end(video::kVideoExtensions),
                       [&](std::string_view v) { return v == ext; });
}

std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return text.str();
}

// `video`'s offsets as just saved: the playing video moves too, if it's this
// one (in game, behind the screen)
void ApplyIfPlaying(const fs::path& video) {
    uint64_t song = 0;
    const std::optional<video::VideoFile> playing = video::CurrentMusicVideo(song);
    std::error_code ec;
    if (!playing || !fs::equivalent(playing->path, video, ec)) return;
    const std::string ini = video::ReadIni(video);
    video::SetMusicVideoSegments(song, video::FindStartTime(ini).value_or(0.0),
                                 video::ParseSegments(video::FindIniValue(ini, "video_segments").value_or("")));
}

void SDLCALL OnFilePicked(void* userdata, const char* const* files, int) {
    auto* holder = static_cast<std::shared_ptr<VideoLibrary::FilePick>*>(userdata);
    {
        VideoLibrary::FilePick& pick = **holder;
        std::lock_guard lock(pick.mutex);
        pick.done = true;
        if (!files) pick.error = SDL_GetError();
        else if (files[0]) pick.file = files[0];
    }
    delete holder;
}

video::SongInfo InfoOf(const VideoLibrary::Song& s) { return {s.shortname, s.title, s.artist, s.length_ms}; }

}

VideoLibrary::VideoLibrary() = default;
VideoLibrary::~VideoLibrary() = default;

void VideoLibrary::Refresh(const std::vector<fs::path>& folders) {
    TakeFilePick();
    const auto now = std::chrono::steady_clock::now();
    if (now - songs_read_ > kSongsEvery) ReadSongs();
    // a job just ended: its song's video is looked for now
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.done != jobs_done_) {
        jobs_done_ = p.done;
        videos_read_ = {};
    }
    if (folders != folders_ || now - videos_read_ > kVideosEvery) {
        folders_ = folders;
        ReadVideos();
    }
}

void VideoLibrary::ReadSongs() {
    songs_read_ = std::chrono::steady_clock::now();
    // the packages' songs as last read: those of the last run before the game boots
    std::vector<Song> songs;
    std::set<std::string> seen;
    for (const content::PackageSongs& package : content::PackageSongsNow(std::chrono::milliseconds(0))) {
        for (const content::DtaSong& s : package.songs) {
            // the first of a shortname is the one a video's named for
            if (s.shortname.empty() || !seen.insert(Lower(s.shortname)).second) continue;
            songs.push_back({.shortname = s.shortname,
                             .title = s.title.empty() ? s.shortname : s.title,
                             .artist = s.artist,
                             .length_ms = s.song_length});
        }
    }
    // the game's own songs in none of the packages (the disc's, exports), as
    // it last listed them
    for (const content::ListedSong& s : content::LastGameSongs()) {
        if (!seen.insert(Lower(s.shortname)).second) continue;
        songs.push_back({.shortname = s.shortname,
                         .title = s.title.empty() ? s.shortname : s.title,
                         .artist = s.artist,
                         .length_ms = s.length_ms,
                         .origin = std::string(content::OriginName(s.origin)),
                         .in_package = false});
    }
    std::sort(songs.begin(), songs.end(), [](const Song& a, const Song& b) { return Lower(a.title) < Lower(b.title); });
    songs_ = std::move(songs);
    videos_read_ = {};  // matched to the videos again
}

void VideoLibrary::ReadVideos() {
    videos_read_ = std::chrono::steady_clock::now();
    videos_.clear();
    std::set<std::string> stems;
    for (const fs::path& folder : folders_) {
        std::error_code ec;
        for (const auto& item : fs::directory_iterator(folder, ec)) {
            if (!item.is_regular_file(ec) || !IsVideo(item.path())) continue;
            Video v;
            v.path = item.path();
            v.stem = rex::path_to_utf8(item.path().stem());
            // as FindVideo looks: the first folder's first
            if (!stems.insert(Lower(v.stem)).second) continue;
            const std::string ini = ReadText(video::IniFor(v.path));
            v.offset = video::FindStartTime(ini);
            // the file clock's time as the system's, through both clocks' now
            const auto written = fs::last_write_time(v.path, ec);
            v.written = std::chrono::duration_cast<std::chrono::seconds>(
                            (std::chrono::system_clock::now() +
                             std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                 written - fs::file_time_type::clock::now()))
                                .time_since_epoch())
                            .count();
            v.segments = video::ParseSegments(video::FindIniValue(ini, "video_segments").value_or(""));
            v.pinned = video::FindIniValue(ini, "video_pinned").value_or("") == "1";
            v.source = video::FindIniValue(ini, "video_source").value_or("");
            v.result = video::LoadResult(video::ResultPath(v.path));
            videos_.push_back(std::move(v));
        }
    }
    std::map<std::string, int> by_stem;
    for (size_t i = 0; i < videos_.size(); i++) by_stem[Lower(videos_[i].stem)] = int(i);
    std::set<int> matched;
    for (Song& s : songs_) {
        const auto it = by_stem.find(Lower(s.shortname));
        s.video = it == by_stem.end() ? -1 : it->second;
        if (s.video >= 0) matched.insert(s.video);
    }
    unmatched_.clear();
    for (int i = 0; i < int(videos_.size()); i++) {
        if (!matched.count(i)) unmatched_.push_back(i);
    }
}

int VideoLibrary::WithVideos() const {
    return int(std::count_if(songs_.begin(), songs_.end(), [](const Song& s) { return s.video >= 0; }));
}

VideoLibrary::State VideoLibrary::StateOf(const Song& s) const {
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (p.running && p.song == s.shortname) return State::kFinding;
    if (video::SongVideoQueued(s.shortname)) return State::kWaiting;
    if (s.video >= 0) {
        const Video& v = videos_[size_t(s.video)];
        if (v.result && v.result->confident && v.offset && std::abs(*v.offset - v.result->offset) < 0.0015) {
            return State::kSynced;
        }
        if (v.result && !v.result->confident && !v.offset) return State::kNotSure;
        if (v.offset) return State::kSetByHand;
        return State::kNotSynced;
    }
    if (const auto end = video::SongVideoEndOf(s.shortname)) {
        if (*end == FetchEnd::kReview) return State::kToReview;
        if (*end == FetchEnd::kFailed) return State::kFailed;
        if (*end == FetchEnd::kNoVideo) return State::kNoneFound;
    }
    return State::kNoVideo;
}

std::optional<float> VideoLibrary::FindingShare(const Song& s) const {
    const video::VideoJobsProgress p = video::VideoJobsNow();
    if (!p.running || p.song != s.shortname) return std::nullopt;
    return float(p.share);
}

void VideoLibrary::SetOffset(Video& v, int ms) {
    video::WriteStartTime(v.path, ms / 1000.0);
    ApplyIfPlaying(v.path);
    v.offset = ms / 1000.0;
}

void VideoLibrary::UseGuess(Video& v) {
    if (!v.result) return;
    // one offset throughout
    video::WriteIniValues(v.path, {{"video_start_time", std::to_string(std::lround(v.result->offset * 1000.0))},
                                   {"video_segments", std::nullopt}});
    v.segments.clear();
    ApplyIfPlaying(v.path);
    v.offset = v.result->offset;
}

void VideoLibrary::MergeParts(Video& v) {
    v.segments.clear();
    video::WriteIniValues(v.path, {{"video_segments", std::nullopt}});
    ApplyIfPlaying(v.path);
}

void VideoLibrary::Sync(const Video& v) {
    if (Syncing()) return;
    sync_ = std::make_shared<SyncJob>();
    sync_->video = v.path;
    sync_taken_ = false;
    std::thread([job = sync_] {
        std::string error;
        const std::optional<video::Match> m = video::AlignVideo(job->video, {}, true, error);
        std::lock_guard lock(job->mutex);
        if (m) job->result = m->result;
        job->error = error;
        job->done = true;
    }).detach();
}

bool VideoLibrary::Syncing() const {
    if (!sync_) return false;
    std::lock_guard lock(sync_->mutex);
    return !sync_->done;
}

bool VideoLibrary::SyncingThis(const Video& v) const { return Syncing() && sync_->video == v.path; }

std::optional<std::string> VideoLibrary::TakeSyncMessage() {
    if (!sync_ || sync_taken_) return std::nullopt;
    std::lock_guard lock(sync_->mutex);
    if (!sync_->done) return std::nullopt;
    sync_taken_ = true;
    videos_read_ = {};
    if (!sync_->result) return sync_->error.empty() ? std::string("Couldn't sync it") : sync_->error;
    if (sync_->result->confident) {
        ApplyIfPlaying(sync_->video);
        return sync_->result->parts > 1
                   ? std::format("Synced: {} parts, the first at {:+.0f} ms", sync_->result->parts,
                                 sync_->result->offset * 1000)
                   : std::format("Synced at {:+.0f} ms", sync_->result->offset * 1000);
    }
    return std::format("Not sure: the best guess was {:+.0f} ms, not saved", sync_->result->offset * 1000);
}

void VideoLibrary::Delete(const Video& v, const std::string& song) {
    std::error_code ec;
    for (const fs::path& p : {v.path, video::IniFor(v.path), video::SongEnvelopePath(v.path),
                              video::VideoEnvelopePath(v.path), video::ResultPath(v.path)}) {
        fs::remove(p, ec);
    }
    if (!song.empty()) video::ForgetSongVideoStatus(song);
    videos_read_ = {};
}

void VideoLibrary::Find(const Song& song, video::VideoJob job) {
    if (folders_.empty()) return;
    job.song = InfoOf(song);
    job.folder = folders_.front();
    video::QueueVideoJobs({std::move(job)});
}

std::vector<const VideoLibrary::Song*> VideoLibrary::BatchSongs() const {
    std::vector<const Song*> out;
    for (const Song& s : songs_) {
        if (s.video >= 0 || video::SongVideoQueued(s.shortname)) continue;
        const auto end = video::SongVideoEndOf(s.shortname);
        if (end == FetchEnd::kNoVideo || end == FetchEnd::kReview) continue;
        out.push_back(&s);
    }
    return out;
}

void VideoLibrary::FindBatch() {
    if (folders_.empty()) return;
    std::vector<video::VideoJob> jobs;
    for (const Song* s : BatchSongs()) {
        video::VideoJob job;
        job.song = InfoOf(*s);
        job.folder = folders_.front();
        jobs.push_back(std::move(job));
    }
    video::QueueVideoJobs(std::move(jobs));
}

void VideoLibrary::PickFile(const Song& song) {
    if (pick_) return;
    pick_ = std::make_shared<FilePick>();
    pick_->song = song;
    static constexpr SDL_DialogFileFilter kVideoFiles[] = {{"Videos", "mp4;m4v;mov;mkv;webm;avi;wmv"}};
    auto* holder = new std::shared_ptr<FilePick>(pick_);
    SDL_ShowOpenFileDialog(OnFilePicked, holder, nullptr, kVideoFiles, 1, nullptr, false);
}

void VideoLibrary::TakeFilePick() {
    if (!pick_) return;
    std::optional<std::string> file;
    Song song;
    {
        std::lock_guard lock(pick_->mutex);
        if (!pick_->done) return;
        file = pick_->file;
        song = pick_->song;
    }
    pick_.reset();
    if (!file) return;
    video::VideoJob job;
    job.kind = video::VideoJob::Kind::kFile;
    job.file = rex::to_path(*file);
    Find(song, std::move(job));
}

}
