#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/Video/sync_align.h"
#include "src/Video/video_jobs.h"

namespace band3::launcher {

// The Videos tab (settings_page.cpp's DrawVideos): a bar of yt-dlp's state,
// finding videos for many songs at once, and the tab's settings (in a popup);
// under it the songs in band3's packages and the game's own (as last read, so
// before the game boots too) on the left, and the one picked on the right:
// its music video in music_videos_folder if it has one, its offset
// (video_start_time, editable), how it got it, and Align
// (src/Video/auto_sync.h), and finding one (src/Video/video_jobs.h), from a
// search whose results can be chosen from, a link or a file of the player's.
// Videos named after no song band3 knows are the list's last filter. Draw
// with the launcher's style applied.
class VideosTab {
public:
    // what the tab borrows from the settings page: its settings' rows, the
    // description of the one pointed at, and that's height
    struct Page {
        std::function<void()> settings;
        std::function<void()> description;
        float description_height = 0;
    };

    VideosTab();
    ~VideosTab();
    // fills the space left
    void Draw(const std::vector<std::filesystem::path>& folders, const Page& page);

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
    void DrawTopBar(const std::vector<std::filesystem::path>& folders, const Page& page);
    void DrawYtDlp();
    void DrawBatch(const std::vector<std::filesystem::path>& folders);
    void DrawSettingsPopup(const Page& page);
    void DrawStatus();
    void DrawFilters();
    void DrawList();
    void DrawLooseList();
    void DrawDetails(const std::vector<std::filesystem::path>& folders);
    void DrawLooseDetails(Video& v);
    void DrawVideoControls(Video& v);
    void DrawParts(Video& v);
    void DrawDelete(Video& v, const std::string& song);
    void StartAlign(const Video& v);
    void TakeAlign();
    void TakeFilePick(const std::vector<std::filesystem::path>& folders);
    bool Shown(const Song& s) const;
    Song* Selected();
    Video* SelectedLoose();
    // a song, or a video named after none, picked in the list
    void Select(const std::string& song, const std::filesystem::path& video);

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
    std::filesystem::path selected_video_;  // or a video named after no song
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
