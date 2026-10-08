#pragma once

#include <algorithm>
#include <cstddef>
#include <deque>
#include <memory>
#include <optional>

#include "src/Video/movie_planes.h"

// The music video's frames between its decoder's thread, which reads them
// ahead of the song, and the game's, which shows the one the song's time is
// at; and the rules for what the decoder does next. Not thread-safe: the
// player (music_video.h) locks around it. Kept apart for unit tests.

namespace band3::video {

struct QueuedFrame {
    double time = 0.0;  // seconds into the video
    std::shared_ptr<const PlaneSet> planes;
};

// frames read ahead of the song's time
inline constexpr size_t kFramesAhead = 3;
// further ahead than this, or behind what's queued by more than kBehind, the
// decoder seeks instead of reading on; while it reads up from a key frame to
// where it sought, only further than kSeekAheadCatchingUp, or a long gap
// between key frames on a slow decode would send it back to the same key
// frame over and over
inline constexpr double kSeekAhead = 1.5;
inline constexpr double kSeekAheadCatchingUp = 10.0;
inline constexpr double kBehind = 0.25;
// a frame is shown from this much before its time (the song's clock and the
// game's frames don't line up exactly)
inline constexpr double kEarly = 0.02;
// frames closer together than this are read but not made into planes: the
// video venues draw their world at 24 or 30 fps
inline constexpr double kMinFrameGap = 1.0 / 35.0;
// while catching up, one frame this far apart is made anyway, so something
// moves if the decoder can't keep up
inline constexpr double kCatchUpGap = 0.25;

class FrameQueue {
public:
    // The frame for video time t: the last at or before it, dropping the
    // ones before that; the last one stays once the video ends. None while
    // the first is still ahead of t, or nothing's queued.
    std::shared_ptr<const PlaneSet> Show(double t) {
        while (frames_.size() >= 2 && frames_[1].time <= t + kEarly) frames_.pop_front();
        if (frames_.empty() || frames_.front().time > t + kEarly) return nullptr;
        return frames_.front().planes;
    }
    void Push(QueuedFrame f) { frames_.push_back(std::move(f)); }
    void Clear() { frames_.clear(); }
    size_t Size() const { return frames_.size(); }
    // frames after t
    size_t AheadOf(double t) const {
        size_t n = 0;
        for (const auto& f : frames_) n += f.time > t ? 1 : 0;
        return n;
    }
    std::optional<double> FirstTime() const {
        return frames_.empty() ? std::nullopt : std::optional<double>(frames_.front().time);
    }

private:
    std::deque<QueuedFrame> frames_;
};

enum class DecodeStep { kSeek, kRead, kWait };

// What the decoder does next for the song at video time `target`: `read` is
// the time of the last frame it read since opening or seeking (none yet),
// `first_queued` the queue's first frame's, `ahead` the frames queued after
// target, `catching_up` whether it's still reading up to where it last
// sought. It seeks when the song jumped back before what's queued (a
// restart, practice's sections) or far ahead of what it read, reads until
// kFramesAhead are queued, then waits.
inline DecodeStep NextStep(double target, std::optional<double> read,
                           std::optional<double> first_queued, size_t ahead,
                           bool catching_up = false) {
    const double ahead_limit = catching_up ? kSeekAheadCatchingUp : kSeekAhead;
    if (target > (read ? *read : 0.0) + ahead_limit) return DecodeStep::kSeek;
    // before the video's start counts as its start, which a seek can't get
    // before
    const std::optional<double> earliest = first_queued ? first_queued : read;
    if (earliest && std::max(target, 0.0) < *earliest - kBehind) return DecodeStep::kSeek;
    return ahead >= kFramesAhead ? DecodeStep::kWait : DecodeStep::kRead;
}

// Whether a frame read at `time` is made into planes and queued, `made` the
// time of the last one that was since opening or seeking (none yet).
inline bool ShouldMake(double time, double target, std::optional<double> made) {
    if (!made) return true;
    const double gap = time - *made;
    if (gap < kMinFrameGap) return false;
    // still catching up to the song: only now and then
    if (time < target - kEarly && gap < kCatchUpGap) return false;
    return true;
}

}
