#include "src/Video/video_files.h"

#include <charconv>
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

double ParseStartTime(std::string_view ini) {
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
    return 0.0;
}

std::optional<VideoFile> FindVideo(const std::vector<std::filesystem::path>& folders,
                                   std::string_view shortname) {
    if (!PlainName(shortname)) return std::nullopt;
    const std::string name(shortname);
    for (const auto& folder : folders) {
        for (std::string_view ext : kVideoExtensions) {
            std::filesystem::path path = folder / (name + std::string(ext));
            if (!IsFile(path)) continue;
            VideoFile file{path, 0.0};
            std::ifstream ini(folder / (name + ".ini"), std::ios::binary);
            if (ini) {
                std::stringstream text;
                text << ini.rdbuf();
                file.start_time = ParseStartTime(text.str());
            }
            return file;
        }
    }
    return std::nullopt;
}

}
