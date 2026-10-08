#pragma once

#include <filesystem>
#include <optional>
#include <string>
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
    // whether the .ini sets it (auto-sync leaves a set one alone)
    bool start_time_set = false;
};

// the .ini beside a video: <shortname>.ini
inline std::filesystem::path IniFor(const std::filesystem::path& video) {
    std::filesystem::path ini = video;
    ini.replace_extension(".ini");
    return ini;
}

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
std::optional<double> FindStartTime(std::string_view ini);

// The .ini's text with video_start_time set to `seconds` (whole
// milliseconds): its line replaced where there is one, else added under
// [song] (made if missing); the rest kept as it was, line endings too.
std::string WithStartTime(std::string_view ini, double seconds);

// WithStartTime written over the video's .ini; false if it can't be
bool WriteStartTime(const std::filesystem::path& video, double seconds);

}
