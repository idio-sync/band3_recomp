#pragma once
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "src/Video/sync_align.h"
#include "src/Video/video_jobs.h"

// The music videos the Music Videos screen shows, apart from drawing them:
// the songs in band3's packages and the game's own (as last read, so before
// the game boots too), each matched with its video in the videos folders by
// shortname, and what can be done to one: find it (video_jobs.h), from a
// search, a link or a file; sync it with its song (auto_sync.h); move its
// offset; or delete it. Videos named after no song band3 knows are kept
// apart. UI thread; the jobs and syncs run on their own threads.

namespace band3::launcher {

class VideoLibrary {
public:
    struct Video {
        std::filesystem::path path;
        // the song's shortname, as named
        std::string stem;
        // video_start_time, the video's time at the song's start, if set
        std::optional<double> offset;
        // what auto-sync came to, as kept beside it
        std::optional<video::SyncResult> result;
        // chosen by hand: searches for many songs leave it alone
        bool pinned = false;
        // the page it came from
        std::string source;
        // when its file was written, in seconds since 1970
        int64_t written = 0;
        // video_segments: its parts after the first (sync_align.h)
        std::vector<video::VideoSegment> segments;
    };
    struct Song {
        std::string shortname;
        std::string title;
        std::string artist;
        int length_ms = 0;
        // for a song in none of band3's packages (the disc's, an export):
        // the game it came from, as the game last listed it
        std::string origin;
        bool in_package = true;
        // into Videos(), or -1
        int video = -1;
    };

    // how a song's video stands, for its row
    enum class State {
        kSynced,
        kNotSure,
        kSetByHand,
        kNotSynced,
        kFinding,
        kWaiting,
        kToReview,
        kFailed,
        kNoneFound,
        kNoVideo,
    };

    VideoLibrary();
    ~VideoLibrary();

    // once a frame: the songs again every 10 s, the folders every 2 s, when
    // they change, and when a job has ended
    void Refresh(const std::vector<std::filesystem::path>& folders);

    const std::vector<Song>& Songs() const { return songs_; }
    const std::vector<Video>& Videos() const { return videos_; }
    // the videos named after no song band3 knows, by index into Videos()
    const std::vector<int>& Unmatched() const { return unmatched_; }
    int WithVideos() const;

    State StateOf(const Song& song) const;
    // the share of the running job, 0 to 1, when it's this song's
    std::optional<float> FindingShare(const Song& song) const;

    // sets the video's offset in its .ini (and on the playing video, if it's it)
    void SetOffset(Video& video, int ms);
    // takes auto-sync's guess it wasn't sure of, one offset throughout
    void UseGuess(Video& video);
    // plays its parts at the first part's offset: for a change of offset that wasn't one
    void MergeParts(Video& video);
    // syncs it with its song again; one at a time
    void Sync(const Video& video);
    bool Syncing() const;
    bool SyncingThis(const Video& video) const;
    // what the last sync came to, once, when it's done
    std::optional<std::string> TakeSyncMessage();
    // the video, its offset and what auto-sync kept of both
    void Delete(const Video& video, const std::string& song);

    // finds a song's video: a search (job.kind kFind), a link or a file; a
    // song with a video keeps it until the new one's in
    void Find(const Song& song, video::VideoJob job);
    // the songs a batch would find videos for: those without one that no
    // search has given up on or left to review
    std::vector<const Song*> BatchSongs() const;
    void FindBatch();

    // opens the system's file window for a song's video file
    void PickFile(const Song& song);

    // what the file window shares with its callback
    struct FilePick;
    struct SyncJob;

private:
    void ReadSongs();
    void ReadVideos();
    void TakeFilePick();

    std::vector<std::filesystem::path> folders_;
    std::vector<Song> songs_;
    std::vector<Video> videos_;
    std::vector<int> unmatched_;
    std::chrono::steady_clock::time_point songs_read_{};
    std::chrono::steady_clock::time_point videos_read_{};
    size_t jobs_done_ = 0;
    std::shared_ptr<SyncJob> sync_;
    bool sync_taken_ = true;
    std::shared_ptr<FilePick> pick_;
};

}
