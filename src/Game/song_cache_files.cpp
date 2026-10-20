#include "song_cache_files.h"
#include <array>
#include <fstream>
#include <string>
#include <system_error>

namespace band3::song_cache {

namespace {

// the song cache the game writes for a new profile: format 13, no songs
constexpr std::array<char, 32> kEmptyCache = {0x0d};

}

bool IsSongCache(std::string_view file_name) {
    return file_name == "songcache" || file_name == "rbdxcache";
}

std::vector<std::filesystem::path> RestoreMissingCaches(const std::filesystem::path& user_data_root,
                                                        std::string_view title_id,
                                                        std::string_view content_type,
                                                        std::string_view file_name) {
    namespace fs = std::filesystem;
    // <root>/<profile XUID>/<title ID>/<content type>/<file name>, as the SDK
    // keeps content; the cache is the file of the package's name inside it
    const fs::path title(title_id);
    const fs::path type(content_type);
    const std::string name(file_name);
    std::vector<fs::path> written;
    std::error_code ec;
    for (const auto& profile : fs::directory_iterator(user_data_root, ec)) {
        if (!profile.is_directory(ec)) continue;
        const fs::path package = profile.path() / title / type / name;
        if (!fs::is_directory(package, ec)) continue;
        const fs::path cache = package / name;
        if (fs::exists(cache, ec) || ec) continue;
        std::ofstream out(cache, std::ios::binary);
        out.write(kEmptyCache.data(), kEmptyCache.size());
        out.close();
        if (out) written.push_back(cache);
    }
    return written;
}

}
