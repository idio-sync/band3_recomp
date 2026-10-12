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

namespace band3::launcher {

// The Videos tab's list, after its settings (settings_page.cpp's
// DrawVideos): the songs in band3's packages (as last read, so before the
// game boots too), each with its music video in music_videos_folder if it
// has one: its offset (video_start_time, editable), how it got it, and Align
// (src/Video/auto_sync.h). Finding videos (src/Video/video_jobs.h): for the
// songs without one at once, or for one song, whose search's results can be
// chosen from, or from a link or a file of the player's. Videos named after
// no song band3 knows are listed after. Draw with the launcher's style
// applied.
class VideosTab {
public:
    VideosTab();
    ~VideosTab();
    void Draw(const std::vector<std::filesystem::path>& folders);

    // an Align, or a file dialog's answer, on a thread of its own
    struct Job;
    struct FilePick;

private:
    struct Video {
        std::filesystem::path path;
        std::string stem;  // the song's shortname, as named
        std::optional<double> offset;
        std::optional<video::SyncResult> result;
        bool pinned = false;
        std::string source;
        int ms = 0;  // the offset field's value
        int64_t written = 0;  // when its file was, in seconds since 1970
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
        std::string lower;  // title, artist and shortname, lower case, to search
        int video = -1;     // into videos_
    };
    void RefreshSongs();
    void RefreshVideos(const std::vector<std::filesystem::path>& folders);
    void DrawYtDlp();
    void DrawBatch(const std::vector<std::filesystem::path>& folders);
    void DrawFilters();
    void DrawSongs();
    void DrawDetails(const std::vector<std::filesystem::path>& folders);
    void DrawVideoControls(Video& v);
    void DrawParts(Video& v);
    void DrawLoose();
    void StartAlign(const Video& v);
    void TakeAlign();
    void TakeFilePick(const std::vector<std::filesystem::path>& folders);
    bool Shown(const Song& s) const;
    Song* Selected();

    std::vector<std::filesystem::path> folders_;
    std::vector<Song> songs_;
    std::vector<Video> videos_;
    std::vector<int> loose_;  // videos named after no song
    std::chrono::steady_clock::time_point songs_read_{};
    std::chrono::steady_clock::time_point videos_read_{};
    std::shared_ptr<Job> job_;
    std::string job_message_;
    std::shared_ptr<FilePick> pick_;
    std::string selected_;  // a song's shortname
    std::string search_;
    int filter_ = 0;
    int scope_ = 0;
    std::string link_;
    std::string ytdlp_setting_;  // music_video_ytdlp_path as last checked
    bool confirm_delete_ = false;
    // a job that would replace the selected song's video, waiting for a yes
    std::optional<video::VideoJob> confirm_replace_;
    size_t jobs_done_ = 0;  // VideoJobsProgress' done, as last seen
};

}
