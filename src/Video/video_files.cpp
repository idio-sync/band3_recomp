#include "src/Video/video_files.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <fstream>
#include <sstream>
#include <string>
#include <system_error>

namespace band3::video {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return s;
}

bool IsFile(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

// a shortname that names a file in the folder, not a path out of it
bool PlainName(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_of("/\\:") == std::string_view::npos;
}

}

double ParseStartTime(std::string_view ini) { return FindStartTime(ini).value_or(0.0); }

std::optional<std::string> FindIniValue(std::string_view ini, std::string_view key) {
    while (!ini.empty()) {
        const size_t end = ini.find('\n');
        std::string_view line = Trim(ini.substr(0, end));
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || Trim(line.substr(0, eq)) != key) continue;
        return std::string(Trim(line.substr(eq + 1)));
    }
    return std::nullopt;
}

std::optional<double> FindStartTime(std::string_view ini) {
    const std::optional<std::string> value = FindIniValue(ini, "video_start_time");
    if (!value) return std::nullopt;
    double ms = 0;
    const auto [ptr, ec] = std::from_chars(value->data(), value->data() + value->size(), ms);
    if (ec != std::errc()) return std::nullopt;
    return ms / 1000.0;
}

std::string WithIniValues(std::string_view ini, const std::vector<IniValue>& values) {
    const std::string nl = ini.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const auto line = [&](const IniValue& v) { return v.first + " = " + *v.second + nl; };
    std::vector<bool> done(values.size(), false);
    std::string out;
    size_t song_end = std::string::npos;  // where [song]'s header line ends in out
    std::string_view rest = ini;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        std::string_view raw = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        const std::string_view t = Trim(raw);
        const size_t eq = t.find('=');
        const std::string_view key = eq == std::string_view::npos ? "" : Trim(t.substr(0, eq));
        size_t i = 0;
        while (i < values.size() && (key.empty() || values[i].first != key)) i++;
        if (i < values.size()) {
            // the first line with the key gets the value; any later one goes
            if (values[i].second && !done[i]) out += line(values[i]);
            done[i] = true;
            continue;
        }
        out += std::string(raw) + nl;
        if (t == "[song]" || t == "[Song]") song_end = out.size();
    }
    std::string added;
    for (size_t i = 0; i < values.size(); i++)
        if (!done[i] && values[i].second) added += line(values[i]);
    if (added.empty()) return out;
    if (song_end == std::string::npos) return "[song]" + nl + added + out;
    out.insert(song_end, added);
    return out;
}

std::string WithStartTime(std::string_view ini, double seconds) {
    return WithIniValues(ini, {{"video_start_time", std::to_string(std::llround(seconds * 1000.0))}});
}

bool WriteIniValues(const std::filesystem::path& video, const std::vector<IniValue>& values) {
    const std::filesystem::path ini = IniFor(video);
    std::string text;
    if (std::ifstream in(ini, std::ios::binary); in) {
        std::stringstream s;
        s << in.rdbuf();
        text = s.str();
    }
    std::ofstream out(ini, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << WithIniValues(text, values);
    return bool(out);
}

bool WriteStartTime(const std::filesystem::path& video, double seconds) {
    return WriteIniValues(video,
                          {{"video_start_time", std::to_string(std::llround(seconds * 1000.0))}});
}

std::vector<VideoSegment> ParseSegments(std::string_view value) {
    std::vector<VideoSegment> out;
    while (!value.empty()) {
        const size_t comma = value.find(',');
        const std::string_view part = Trim(value.substr(0, comma));
        value.remove_prefix(comma == std::string_view::npos ? value.size() : comma + 1);
        const size_t colon = part.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view start = Trim(part.substr(0, colon)), offset = Trim(part.substr(colon + 1));
        double ms = 0;
        if (start.empty() || std::from_chars(start.data(), start.data() + start.size(), ms).ec != std::errc() ||
            ms < 0)
            continue;
        VideoSegment seg;
        seg.song_start = ms / 1000.0;
        if (offset != "gap") {
            double off = 0;
            if (offset.empty() ||
                std::from_chars(offset.data(), offset.data() + offset.size(), off).ec != std::errc())
                continue;
            seg.offset = off / 1000.0;
        }
        out.push_back(seg);
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const VideoSegment& a, const VideoSegment& b) { return a.song_start < b.song_start; });
    return out;
}

std::string FormatSegments(const std::vector<VideoSegment>& later) {
    std::string out;
    for (const VideoSegment& s : later) {
        if (!out.empty()) out += ", ";
        out += std::to_string(std::llround(s.song_start * 1000.0)) + ":" +
               (s.offset ? std::to_string(std::llround(*s.offset * 1000.0)) : "gap");
    }
    return out;
}

size_t SegmentAt(const VideoFile& file, double song_time) {
    size_t k = 0;
    while (k < file.segments.size() && file.segments[k].song_start <= song_time) k++;
    return k;
}

std::optional<double> VideoTimeAt(const VideoFile& file, double song_time) {
    const size_t k = SegmentAt(file, song_time);
    const std::optional<double> offset = k == 0 ? std::optional<double>(file.start_time)
                                                : file.segments[k - 1].offset;
    if (!offset) return std::nullopt;
    return song_time + *offset;
}

std::string ReadIni(const std::filesystem::path& video) {
    std::ifstream in(IniFor(video), std::ios::binary);
    if (!in) return {};
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}

bool SplitMap(const SegmentMap& map, double& start, std::vector<VideoSegment>& later) {
    // a gap before the first part: the video hasn't begun, as before any
    // video_start_time's start
    size_t first = 0;
    while (first < map.segments.size() && !map.segments[first].offset) first++;
    if (first == map.segments.size()) return false;
    start = *map.segments[first].offset;
    later.assign(map.segments.begin() + long(first) + 1, map.segments.end());
    return true;
}

bool WriteSegmentMap(const std::filesystem::path& video, const SegmentMap& map) {
    double start = 0;
    std::vector<VideoSegment> later;
    if (!SplitMap(map, start, later)) return false;
    return WriteIniValues(
        video, {{"video_start_time", std::to_string(std::llround(start * 1000.0))},
                {"video_segments", later.empty() ? std::nullopt : std::optional<std::string>(FormatSegments(later))}});
}

bool WriteMatch(const std::filesystem::path& video, const Match& match) {
    if (match.map && match.result.confident) return WriteSegmentMap(video, *match.map);
    return WriteIniValues(video, {{"video_start_time", std::to_string(std::llround(match.result.offset * 1000.0))},
                                  {"video_segments", std::nullopt}});
}

std::optional<VideoFile> FindVideo(const std::vector<std::filesystem::path>& folders,
                                   std::string_view shortname) {
    if (!PlainName(shortname)) return std::nullopt;
    const std::string name(shortname);
    for (const auto& folder : folders) {
        for (std::string_view ext : kVideoExtensions) {
            std::filesystem::path path = folder / (name + std::string(ext));
            if (!IsFile(path)) continue;
            VideoFile file{path, 0.0, false};
            std::ifstream ini(folder / (name + ".ini"), std::ios::binary);
            if (ini) {
                std::stringstream s;
                s << ini.rdbuf();
                const std::string text = s.str();
                const std::optional<double> start = FindStartTime(text);
                file.start_time = start.value_or(0.0);
                file.start_time_set = start.has_value();
                file.source = FindIniValue(text, "video_source").value_or("");
                file.pinned = FindIniValue(text, "video_pinned").value_or("") == "1";
                file.segments = ParseSegments(FindIniValue(text, "video_segments").value_or(""));
            }
            return file;
        }
    }
    return std::nullopt;
}

}
