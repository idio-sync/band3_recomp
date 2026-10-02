#include "paths.h"
#include <system_error>

namespace band3::paths {

namespace {

std::filesystem::path FromUtf8(std::string_view value) {
    return std::filesystem::path(std::u8string(value.begin(), value.end()));
}

}

std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor) {
    if (value.empty()) return {};
    auto path = FromUtf8(value);
    if (path.is_absolute()) return path;
    return (anchor / path).lexically_normal();
}

std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name) {
    std::error_code ec;
    for (const auto& dir : dirs) {
        auto candidate = dir / FromUtf8(name);
        if (std::filesystem::is_regular_file(candidate, ec)) return candidate;
    }
    return {};
}

std::vector<std::string> SplitList(std::string_view value) {
    std::vector<std::string> parts;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find('|', start);
        if (end == std::string_view::npos) end = value.size();
        auto part = value.substr(start, end - start);
        const auto first = part.find_first_not_of(" \t");
        if (first != std::string_view::npos) {
            const auto last = part.find_last_not_of(" \t");
            parts.emplace_back(part.substr(first, last - first + 1));
        }
        start = end + 1;
    }
    return parts;
}

}
