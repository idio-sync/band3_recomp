#include "src/Video/song_audio.h"

#include <algorithm>

namespace band3::video {

namespace {

bool SameName(std::string_view a, std::string_view b) {
    const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; };
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) { return lower(x) == lower(y); });
}

}

std::optional<SongMogg> FindSongMogg(const std::vector<content::PackageSongs>& packages,
                                     std::string_view shortname) {
    if (shortname.empty()) return std::nullopt;
    for (const content::PackageSongs& package : packages) {
        for (const content::DtaSong& song : package.songs) {
            if (!SameName(song.shortname, shortname)) continue;
            std::string mogg = song.song_path.empty()
                                   ? "songs/" + song.shortname + "/" + song.shortname
                                   : song.song_path;
            mogg += ".mogg";
            std::replace(mogg.begin(), mogg.end(), '/', '\\');
            return SongMogg{package.path, std::move(mogg)};
        }
    }
    return std::nullopt;
}

}
