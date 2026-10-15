#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/Video/sync_align.h"
#include "src/Video/video_search.h"
#include "src/Video/ytdlp.h"

// Getting one song's music video into the videos folder, synced: found by a
// search (the likeliest results' sound fetched and matched against the
// song's, the first that's both a likely upload and a sure match taken, its
// picture fetched and checked for a still one), or from a link or a file the
// player chose, which is taken whatever it matches (pinned: never replaced
// by a search). It ends as <shortname>.<ext> with its .ini (video_start_time,
// video_source, video_pinned) and its sync caches (sync_cache.h), the same
// as a video put there by hand. Runs on the caller's thread, which may wait
// minutes; what it needs from band3 comes in FetchSetup, so tests stand in.

namespace band3::video {

enum class FetchStage { kSearching, kFetchingSound, kMatching, kFetchingVideo, kChecking, kSaving };

// a search result as far as it got
struct TriedCandidate {
    ScoredCandidate candidate;
    std::optional<SyncResult> match;  // its sound against the song's, if fetched
    std::string problem;              // why it wasn't taken; "" if not tried
};

enum class FetchEnd {
    kDone,      // saved and synced
    kGuessed,   // saved (the player's choice), its offset only a guess: worth a check
    kUnsynced,  // saved without the song's sound: it syncs as the song first plays
    kReview,    // nothing sure: the candidates are for the player to choose from
    kNoVideo,   // nothing that looks like the song's video
    kFailed,    // `message` says why
    kStopped,   // cancelled
};

struct FetchResult {
    FetchEnd end = FetchEnd::kFailed;
    std::string message;
    std::vector<TriedCandidate> candidates;
    std::filesystem::path video;   // where it's saved
    std::optional<double> offset;  // video_start_time, seconds
    std::string source;            // the link it came from
};

struct FetchSetup {
    const YtDlp* ytdlp = nullptr;
    std::filesystem::path folder;  // the videos folder it goes in
    VideoFormat format;
    bool auto_accept = true;  // false: a search only proposes (kReview)
    bool keep_audio = false;  // keep the sound fetched, in .sync, to align again
    // how much of the song must overlap the video's sound for a match
    // (AlignEnvelopes'); less only for tests' short clips
    double min_overlap = 15.0;
    // the song's rhythm envelope (song_audio.h, or one captured); false,
    // with why, without it
    std::function<bool(const SongInfo&, std::vector<float>&, std::string&)> song_envelope;
    // each stage as it starts (0), and a download's share done (0..1)
    std::function<void(FetchStage, double)> progress;
    const std::atomic<bool>* cancel = nullptr;
};

// how many of a search's likely results have their sound fetched, at most
inline constexpr int kMaxTries = 3;

// A search for the song's video, taken when sure (FetchSetup's auto_accept),
// else its candidates for review. The still check opens video on this
// thread: StartVideoThread (video_decoder.h) first.
FetchResult FindSongVideo(const SongInfo& song, const FetchSetup& setup);
// The video at `url` (a candidate chosen, or a link pasted), pinned.
FetchResult FetchSongVideo(const SongInfo& song, std::string_view url, const FetchSetup& setup);
// A file of the player's, copied in (not moved), pinned.
FetchResult UseVideoFile(const SongInfo& song, const std::filesystem::path& file,
                         const FetchSetup& setup);

// a FetchEnd's words, for the log and the Music Videos screen
std::string_view FetchEndText(FetchEnd end);

}
