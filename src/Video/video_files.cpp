#include "src/Video/video_files.h"

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

std::optional<double> FindStartTime(std::string_view ini) {
    while (!ini.empty()) {
        const size_t end = ini.find('\n');
        std::string_view line = Trim(ini.substr(0, end));
        ini.remove_prefix(end == std::string_view::npos ? ini.size() : end + 1);
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos || Trim(line.substr(0, eq)) != "video_start_time")
            continue;
        const std::string_view value = Trim(line.substr(eq + 1));
        double ms = 0;
        const auto [ptr, ec] = std::from_chars(value.data(), value.data() + value.size(), ms);
        if (ec == std::errc()) return ms / 1000.0;
    }
    return std::nullopt;
}

std::string WithStartTime(std::string_view ini, double seconds) {
    const std::string nl = ini.find("\r\n") != std::string_view::npos ? "\r\n" : "\n";
    const std::string line = "video_start_time = " + std::to_string(std::llround(seconds * 1000.0));
    std::string out;
    bool done = false;
    size_t song_end = std::string::npos;  // where [song]'s header line ends in out
    std::string_view rest = ini;
    while (!rest.empty()) {
        const size_t end = rest.find('\n');
        std::string_view raw = rest.substr(0, end);
        rest.remove_prefix(end == std::string_view::npos ? rest.size() : end + 1);
        if (!raw.empty() && raw.back() == '\r') raw.remove_suffix(1);
        const std::string_view t = Trim(raw);
        const size_t eq = t.find('=');
        if (!done && eq != std::string_view::npos && Trim(t.substr(0, eq)) == "video_start_time") {
            out += line + nl;
            done = true;
            continue;
        }
        out += std::string(raw) + nl;
        if (t == "[song]" || t == "[Song]") song_end = out.size();
    }
    if (done) return out;
    if (song_end == std::string::npos) return "[song]" + nl + line + nl + out;
    out.insert(song_end, line + nl);
    return out;
}

bool WriteStartTime(const std::filesystem::path& video, double seconds) {
    const std::filesystem::path ini = IniFor(video);
    std::string text;
    if (std::ifstream in(ini, std::ios::binary); in) {
        std::stringstream s;
        s << in.rdbuf();
        text = s.str();
    }
    std::ofstream out(ini, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << WithStartTime(text, seconds);
    return bool(out);
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
                std::stringstream text;
                text << ini.rdbuf();
                const std::optional<double> start = FindStartTime(text.str());
                file.start_time = start.value_or(0.0);
                file.start_time_set = start.has_value();
            }
            return file;
        }
    }
    return std::nullopt;
}

}
