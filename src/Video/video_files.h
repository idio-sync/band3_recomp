#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// Where a song's music video is: <shortname>.<ext> in one of the
// music_videos_folder setting's folders, and beside it, optionally,
// <shortname>.ini with Clone Hero's song.ini key video_start_time, the
// video's time in milliseconds at the song's start (negative: the video
// starts that much into the song). band3 also keeps there video_source, the
// link a video was downloaded from, and video_pinned = 1 for one chosen by
// hand, which finding videos for many songs at once never replaces.

namespace band3::video {

struct VideoFile {
    std::filesystem::path path;
    // seconds into the video at the song's start
    double start_time = 0.0;
    // whether the .ini sets it (auto-sync leaves a set one alone)
    bool start_time_set = false;
    std::string source;  // video_source; "" for none
    bool pinned = false;  // video_pinned = 1
};

// a key and its value for an .ini; no value takes the key out
using IniValue = std::pair<std::string, std::optional<std::string>>;

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
// a key's value in an .ini's text, trimmed; none without it
std::optional<std::string> FindIniValue(std::string_view ini, std::string_view key);

// The .ini's text with each key set to its value: its line replaced where
// there is one, else added under [song] (made if missing); a key with no
// value taken out. The rest kept as it was, line endings too.
std::string WithIniValues(std::string_view ini, const std::vector<IniValue>& values);
// video_start_time set to `seconds` (whole milliseconds)
std::string WithStartTime(std::string_view ini, double seconds);

// written over the video's .ini; false if it can't be
bool WriteIniValues(const std::filesystem::path& video, const std::vector<IniValue>& values);
bool WriteStartTime(const std::filesystem::path& video, double seconds);

}
