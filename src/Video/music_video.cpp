#include "src/Video/music_video.h"

#include <rex/logging.h>

#include <algorithm>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "src/Video/frame_queue.h"
#include "src/Video/picture_convert.h"
#include "src/Video/video_decoder.h"
#include "src/Video/video_files.h"
#include "src/config.h"
#include "src/paths.h"
#include "src/settings.h"

namespace band3::video {

namespace {

// The shape the video venues show their movie at: the whole screen, which
// the game draws at 16:9 (wider windows stretch it).
constexpr float kScreenAspect = 16.0f / 9.0f;

// music_videos_folder's, and any more it names separated by '|'; empty (as
// the launcher saves the default folder) is "videos"
std::vector<std::filesystem::path> VideoFolders() {
    const auto anchor = IniAnchor();
    std::vector<std::string> entries = paths::SplitList(REXCVAR_GET(music_videos_folder));
    if (entries.empty()) entries.push_back("videos");
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : entries) folders.push_back(paths::Resolve(entry, anchor));
    return folders;
}

class Player {
public:
    void SetSong(std::string_view shortname) {
        std::lock_guard lock(mutex_);
        // a song's end with none begun: no thread for that
        if (!started_ && shortname.empty()) return;
        if (!started_) {
            started_ = true;
            std::thread(&Player::Run, this).detach();
        }
        Begin(shortname, REXCVAR_GET(music_videos));
    }

    bool Frame(double song_time, const PlaneSizes& sizes, std::shared_ptr<const PlaneSet>& out) {
        std::lock_guard lock(mutex_);
        // turned off, it stops at once; turned on, it starts with the next song
        if (!file_ || !REXCVAR_GET(music_videos)) return false;
        target_ = song_time + file_->start_time;
        sizes_ = sizes;
        fit_ = ParseFit(REXCVAR_GET(music_video_fit));
        std::shared_ptr<const PlaneSet> planes = queue_.Show(target_);
        // it may want to read on, or seek
        wake_.notify_one();
        if (!planes) {
            out = Black(sizes);
        } else if (planes->y.width != sizes.w || planes->y.height != sizes.h ||
                   planes->cr.width != sizes.cw || planes->cr.height != sizes.ch) {
            out = Resized(planes, sizes);
        } else {
            out = std::move(planes);
        }
        return true;
    }

private:
    // a new song (or none): its video looked for
    void Begin(std::string_view shortname, bool on) {
        shortname_ = shortname;
        file_ = on && !shortname.empty() ? FindVideo(VideoFolders(), shortname) : std::nullopt;
        if (on && !shortname.empty()) {
            if (file_)
                REXLOG_INFO("Music video for {}: {} (starts {:.3f} s in)", shortname_,
                            file_->path.string(), file_->start_time);
            else
                REXLOG_INFO("No music video for {} in music_videos_folder", shortname_);
        }
        song_++;
        queue_.Clear();
        wake_.notify_one();
    }

    std::shared_ptr<const PlaneSet> Black(const PlaneSizes& sizes) {
        if (!black_ || black_sizes_ != sizes) {
            auto planes = std::make_shared<PlaneSet>();
            planes->y.Resize(sizes.w, sizes.h, kBlackY);
            planes->cr.Resize(sizes.cw, sizes.ch, kNeutralC);
            planes->cb.Resize(sizes.cw, sizes.ch, kNeutralC);
            black_ = std::move(planes);
            black_sizes_ = sizes;
        }
        return black_;
    }

    // a frame made for other planes, until the thread makes them for these
    std::shared_ptr<const PlaneSet> Resized(const std::shared_ptr<const PlaneSet>& from,
                                            const PlaneSizes& sizes) {
        if (resized_from_ != from || resized_sizes_ != sizes) {
            auto planes = std::make_shared<PlaneSet>();
            planes->y = ResizeNearest(from->y, sizes.w, sizes.h);
            planes->cr = ResizeNearest(from->cr, sizes.cw, sizes.ch);
            planes->cb = ResizeNearest(from->cb, sizes.cw, sizes.ch);
            resized_ = std::move(planes);
            resized_from_ = from;
            resized_sizes_ = sizes;
        }
        return resized_;
    }

    // the decoder's thread: opens each song's video, then reads it ahead of
    // the song's time, seeking when that jumps
    void Run() {
        const bool can_decode = StartVideoThread();
        std::unique_ptr<VideoDecoder> decoder;
        RgbFrame frame;
        uint64_t song = 0;
        std::unique_lock lock(mutex_);
        for (;;) {
            if (song != song_) {
                song = song_;
                queue_.Clear();
                epoch_++;
                read_.reset();
                made_.reset();
                seek_to_.reset();
                ended_ = false;
                const std::optional<std::filesystem::path> path =
                    file_ ? std::optional(file_->path) : std::nullopt;
                // closing a reader can take a while: not with the game waiting
                lock.unlock();
                decoder.reset();
                if (!path) {
                    lock.lock();
                    continue;
                }
                std::string error = "this platform can't decode video";
                std::unique_ptr<VideoDecoder> opened =
                    can_decode ? OpenVideo(*path, error) : nullptr;
                if (!opened) REXLOG_WARN("Music video {}: {}", path->string(), error);
                lock.lock();
                if (song == song_) decoder = std::move(opened);
                continue;
            }
            DecodeStep step = DecodeStep::kWait;
            if (decoder) {
                step = NextStep(target_, read_, queue_.FirstTime(), queue_.AheadOf(target_),
                                seek_to_.has_value());
                // past its end: only a jump back starts it again
                if (ended_ && (step == DecodeStep::kRead || target_ > read_.value_or(0.0)))
                    step = DecodeStep::kWait;
            }
            if (step == DecodeStep::kWait) {
                wake_.wait(lock);
                continue;
            }
            if (step == DecodeStep::kSeek) {
                const double to = std::max(0.0, target_);
                queue_.Clear();
                epoch_++;
                made_.reset();
                read_ = to;
                seek_to_ = to;
                ended_ = false;
                lock.unlock();
                const bool sought = decoder->Seek(to);
                lock.lock();
                // reading on from where it was would only seek again
                if (!sought) {
                    REXLOG_WARN("Music video: seeking to {:.3f} s failed; stopping it", to);
                    ended_ = true;
                }
                continue;
            }
            const uint64_t epoch = epoch_;
            lock.unlock();
            const bool got = decoder->Read(frame);
            lock.lock();
            if (epoch != epoch_ || song != song_) continue;
            if (!got) {
                ended_ = true;
                continue;
            }
            read_ = std::max(read_.value_or(frame.time), frame.time);
            // read from the key frame up to where it sought
            if (seek_to_ && frame.time >= *seek_to_ - kEarly) seek_to_.reset();
            if (!ShouldMake(frame.time, target_, made_)) continue;
            made_ = frame.time;
            const PlaneSizes sizes = sizes_;
            const Fit fit = fit_;
            lock.unlock();
            auto planes = std::make_shared<PlaneSet>();
            ToPlanes(frame, fit, kScreenAspect, sizes.w, sizes.h, sizes.cw, sizes.ch, *planes);
            lock.lock();
            if (epoch != epoch_ || song != song_) continue;
            queue_.Push({frame.time, std::move(planes)});
        }
    }

    std::mutex mutex_;
    std::condition_variable wake_;
    bool started_ = false;

    // the game's side: the song, its video, where the song is
    std::string shortname_;
    std::optional<VideoFile> file_;
    uint64_t song_ = 0;  // counts songs looked up, so the thread sees a new one
    double target_ = 0.0;
    PlaneSizes sizes_;
    Fit fit_ = Fit::kFit;
    std::shared_ptr<const PlaneSet> black_;
    PlaneSizes black_sizes_;
    std::shared_ptr<const PlaneSet> resized_, resized_from_;
    PlaneSizes resized_sizes_;

    // between the two
    FrameQueue queue_;
    uint64_t epoch_ = 0;  // counts opens and seeks, so frames made before go

    // the thread's: the last frame read and the last made into planes since
    // the last open or seek, where the last seek went while it's still
    // reading up to there from the key frame before, and whether it read to
    // the end
    std::optional<double> read_, made_, seek_to_;
    bool ended_ = false;
};

Player& ThePlayer() {
    static Player* player = new Player;  // its thread runs until the game exits
    return *player;
}

}

bool MusicVideosOn() { return REXCVAR_GET(music_videos); }

void SetMusicVideoSong(std::string_view shortname) { ThePlayer().SetSong(shortname); }

bool MusicVideoFrame(double song_time, const PlaneSizes& sizes,
                     std::shared_ptr<const PlaneSet>& out) {
    return ThePlayer().Frame(song_time, sizes, out);
}

}
