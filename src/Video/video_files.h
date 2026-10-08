#pragma once

#include <filesystem>
#include <optional>
#include <string_view>
#include <vector>

// Where a song's music video is: <shortname>.<ext> in one of the
// music_videos_folder setting's folders, and beside it, optionally,
// <shortname>.ini with Clone Hero's song.ini key video_start_time, the
// video's time in milliseconds at the song's start (negative: the video
// starts that much into the song).

namespace band3::video {

struct VideoFile {
    std::filesystem::path path;
    // seconds into the video at the song's start
    double start_time = 0.0;
};

// the extensions looked for, in order
inline constexpr std::string_view kVideoExtensions[] = {".mp4", ".m4v", ".mov", ".mkv",
                                                         ".webm", ".avi", ".wmv"};

// the first folder's video for the song, or none (no shortname, no file);
// its start time from the .ini beside it, 0 without one
std::optional<VideoFile> FindVideo(const std::vector<std::filesystem::path>& folders,
                                   std::string_view shortname);

// video_start_time in an .ini's text, in seconds; 0 without it. A
// `[section]` line and `;` or `#` comments are skipped, as are other keys.
double ParseStartTime(std::string_view ini);

}
