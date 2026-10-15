#pragma once

#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/Video/video_fetch.h"
#include "src/Video/video_job_state.h"

// The queue of songs whose music videos are being found or fetched, for the
// Music Videos screen (launcher and in game): one at a time on a thread of its own
// (video_fetch.h), with the settings as each starts (music_video_ytdlp_path,
// _ytdlp_args, _max_height, _prefer_60fps, _auto_accept, _keep_audio). A
// batch and a single song go through it alike. While a song is being
// played it waits, so a batch never takes the game's time; it goes on in the
// menus. Each song's outcome is kept (video_job_state.h) for review.

namespace band3::video {

struct VideoJob {
    enum class Kind { kFind, kLink, kFile };
    Kind kind = Kind::kFind;
    SongInfo song;
    std::filesystem::path folder;  // the videos folder it goes in
    std::string link;              // kLink's
    std::filesystem::path file;    // kFile's
};

// Adds jobs at the queue's end. A search (kFind) for a song whose video was
// chosen by hand (video_pinned), or one already waiting, is left out; a link
// or a file for a song replaces a job waiting for it. How many were added. A
// job that fails because the network's down or the disk's full (ytdlp.h's
// TroubleOf) pauses the queue, and is put back first for when it goes on.
size_t QueueVideoJobs(std::vector<VideoJob> jobs);
void PauseVideoJobs(bool paused);
// stops the job running and drops those waiting
void StopVideoJobs();

struct VideoJobsProgress {
    bool running = false;    // a job's under way
    bool paused = false;
    std::string paused_why;  // paused by itself: the network's down or the disk's full
    bool waiting_for_song = false;  // a song's being played
    size_t waiting = 0;      // jobs not started
    size_t done = 0;         // of this batch (since the queue was last empty)
    size_t total = 0;
    size_t synced = 0, guessed = 0, review = 0, none = 0, failed = 0;  // this batch's
    std::string song;        // the running job's shortname
    FetchStage stage = FetchStage::kSearching;
    double share = 0.0;      // of its download
};
VideoJobsProgress VideoJobsNow();

// a song's last outcome; none before its first
std::optional<SongVideoStatus> SongVideoStatusOf(std::string_view shortname);
// how it ended, alone: cheap enough for every row of a list
std::optional<FetchEnd> SongVideoEndOf(std::string_view shortname);
// whether a job for it is waiting or running
bool SongVideoQueued(std::string_view shortname);
void ForgetSongVideoStatus(std::string_view shortname);

// yt-dlp as music_video_ytdlp_path finds it, checked on a thread: its
// version, or why it can't be run; `again` checks again (after the setting
// or yt-dlp changed)
struct YtDlpState {
    bool checking = false;
    std::string version;  // "" without
    std::string problem;
    bool updating = false;
    std::string update_output;  // what -U said last
    // a JavaScript runtime yt-dlp can use for YouTube is on PATH (ytdlp.h)
    bool js_runtime = true;
};
YtDlpState YtDlpNow(bool again = false);
// yt-dlp -U on a thread; YtDlpNow says how it went
void UpdateYtDlp();

// what a search, a download or a match is doing, for the screen
std::string_view FetchStageText(FetchStage stage);

}
