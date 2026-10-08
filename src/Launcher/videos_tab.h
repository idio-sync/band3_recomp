#pragma once

#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "src/Video/sync_align.h"

namespace band3::launcher {

// The Videos tab's list, after its settings (settings_page.cpp's
// DrawSection): each music video in music_videos_folder, its offset
// (video_start_time in its .ini, editable), how it got it (auto-sync's
// result in .sync, src/Video/sync_cache.h), and Align, which matches the
// video with its song's sound captured when the song last played with it
// (src/Video/auto_sync.h). Draw with the launcher's style applied.
class VideosTab {
public:
    VideosTab();
    ~VideosTab();
    void Draw(const std::vector<std::filesystem::path>& folders);

private:
    struct Entry {
        std::filesystem::path path;
        std::string name;  // the file's stem: the song's shortname
        std::optional<double> offset;
        std::optional<video::SyncResult> result;
        bool captured = false;  // its song's sound is kept
        int ms = 0;             // the offset field's value
    };
    // an Align running on a thread of its own, shared with it
    struct Job;

    void Refresh(const std::vector<std::filesystem::path>& folders);
    void DrawEntry(Entry& e);
    void StartAlign(const Entry& e);

    std::vector<std::filesystem::path> folders_;
    std::vector<Entry> entries_;
    std::chrono::steady_clock::time_point refreshed_{};
    std::shared_ptr<Job> job_;
    std::string job_message_;
};

}
