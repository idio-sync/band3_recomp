#include "song_inventory.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <map>
#include <set>
#include "src/Net/http_request.h"
#include "src/Net/json.h"

namespace band3::content {

namespace {

using http::JsonString;

int64_t Whole(const json::Value& value) {
    const double d = value.Number(0);
    return std::isfinite(d) ? static_cast<int64_t>(d) : 0;
}

// the times are bigger than a JSON number keeps exactly, so they go as text
int64_t WholeText(const std::string& text) {
    int64_t n = 0;
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), n);
    return ec == std::errc() && end == text.data() + text.size() ? n : 0;
}

// letters and digits, lower-cased, as the page's "Similar in library" goes
std::string Folded(std::string_view text) {
    std::string out;
    for (const char c : text) {
        if (c >= 'A' && c <= 'Z') out += static_cast<char>(c - 'A' + 'a');
        else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) out += c;
    }
    return out;
}

std::string Lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

std::string_view KindName(DuplicateKind kind) {
    switch (kind) {
        case DuplicateKind::kSongId: return "song_id";
        case DuplicateKind::kShortname: return "shortname";
        case DuplicateKind::kSimilar: return "similar";
    }
    return "";
}

}

std::vector<PackageSongs> ParseInventoryCache(std::string_view text) {
    std::vector<PackageSongs> packages;
    const auto json = json::Parse(text);
    if (!json || Whole((*json)["version"]) != 1) return packages;
    for (const json::Value& entry : (*json)["packages"].Items()) {
        PackageSongs package;
        package.path = entry["path"].Text();
        if (package.path.empty()) continue;
        package.size = Whole(entry["size"]);
        package.modified = WholeText(entry["modified"].Text());
        package.unreadable = entry["unreadable"].Bool();
        for (const json::Value& s : entry["songs"].Items()) {
            DtaSong song;
            song.shortname = s["shortname"].Text();
            if (song.shortname.empty()) continue;
            song.song_id = static_cast<int32_t>(std::clamp<int64_t>(Whole(s["song_id"]), INT32_MIN, INT32_MAX));
            song.title = s["title"].Text();
            song.artist = s["artist"].Text();
            package.songs.push_back(std::move(song));
        }
        packages.push_back(std::move(package));
    }
    return packages;
}

std::string FormatInventoryCache(const std::vector<PackageSongs>& packages) {
    std::string out = "{\"version\": 1, \"packages\": [";
    for (size_t i = 0; i < packages.size(); i++) {
        const PackageSongs& p = packages[i];
        out += i ? ",\n  " : "\n  ";
        out += "{\"path\": " + JsonString(p.path) + ", \"size\": " + std::to_string(p.size) +
               ", \"modified\": \"" + std::to_string(p.modified) +
               "\", \"unreadable\": " + (p.unreadable ? "true" : "false") + ", \"songs\": [";
        for (size_t k = 0; k < p.songs.size(); k++) {
            const DtaSong& s = p.songs[k];
            if (k) out += ", ";
            out += "{\"shortname\": " + JsonString(s.shortname) +
                   ", \"song_id\": " + std::to_string(s.song_id) +
                   ", \"title\": " + JsonString(s.title) + ", \"artist\": " + JsonString(s.artist) + "}";
        }
        out += "]}";
    }
    return out + "\n]}\n";
}

std::vector<DuplicateGroup> FindDuplicates(const std::vector<PackageSongs>& packages,
                                           const std::vector<GameSong>& game) {
    std::vector<SongCopy> all;
    for (size_t p = 0; p < packages.size(); p++) {
        for (const DtaSong& song : packages[p].songs) all.push_back({song, static_cast<int32_t>(p), true});
    }

    std::vector<DuplicateGroup> groups;
    // the same song_id: the first loaded is the game's
    std::map<int32_t, std::vector<size_t>> by_id;
    for (size_t i = 0; i < all.size(); i++) {
        if (all[i].song.song_id) by_id[all[i].song.song_id].push_back(i);
    }
    std::vector<SongCopy> loaded;
    for (size_t i = 0; i < all.size(); i++) {
        const int32_t id = all[i].song.song_id;
        if (!id || by_id[id].front() == i) loaded.push_back(all[i]);
    }
    for (const auto& [id, copies] : by_id) {
        if (copies.size() < 2) continue;
        DuplicateGroup group{DuplicateKind::kSongId, std::to_string(id), {}};
        for (const size_t i : copies) {
            SongCopy copy = all[i];
            copy.in_use = i == copies.front();
            group.copies.push_back(std::move(copy));
        }
        groups.push_back(std::move(group));
    }

    // the game's own songs: those whose song_id no package has
    for (const GameSong& song : game) {
        if (song.song_id && by_id.contains(song.song_id)) continue;
        loaded.push_back({DtaSong{song.shortname, song.song_id, song.title, song.artist}, -1, true});
    }

    // among what the game has: the same shortname, then the same artist and title
    std::map<std::string, std::vector<size_t>> by_shortname, by_name;
    for (size_t i = 0; i < loaded.size(); i++) {
        by_shortname[loaded[i].song.shortname].push_back(i);
        const std::string artist = Folded(loaded[i].song.artist), title = Folded(loaded[i].song.title);
        if (!artist.empty() && !title.empty()) by_name[artist + "|" + title].push_back(i);
    }
    std::set<std::vector<size_t>> clashes;
    for (const auto& [shortname, members] : by_shortname) {
        if (members.size() < 2) continue;
        clashes.insert(members);
        DuplicateGroup group{DuplicateKind::kShortname, shortname, {}};
        for (const size_t i : members) group.copies.push_back(loaded[i]);
        groups.push_back(std::move(group));
    }
    for (const auto& [name, members] : by_name) {
        // said already, as a shortname clash
        if (members.size() < 2 || clashes.contains(members)) continue;
        const DtaSong& first = loaded[members.front()].song;
        DuplicateGroup group{DuplicateKind::kSimilar, first.artist + " - " + first.title, {}};
        for (const size_t i : members) group.copies.push_back(loaded[i]);
        groups.push_back(std::move(group));
    }

    std::ranges::stable_sort(groups, {}, [](const DuplicateGroup& g) {
        return std::pair(g.kind, Lower(g.copies.front().song.title));
    });
    return groups;
}

std::string FormatDuplicates(const std::vector<DuplicateGroup>& groups,
                             const std::vector<PackageSongs>& packages,
                             const InventoryStatus& status) {
    const auto unreadable = std::ranges::count_if(packages, [](const auto& p) { return p.unreadable; });
    std::string out = std::string("{\"reading\":") + (status.reading ? "true" : "false") +
                      ",\"read\":" + std::to_string(status.read) +
                      ",\"total\":" + std::to_string(status.total) +
                      ",\"unreadable\":" + std::to_string(unreadable) +
                      ",\"game\":" + (status.game ? "true" : "false") + ",\"groups\":[";
    for (size_t g = 0; g < groups.size(); g++) {
        const DuplicateGroup& group = groups[g];
        if (g) out += ',';
        out += "{\"kind\":" + JsonString(KindName(group.kind)) + ",\"key\":" + JsonString(group.key) +
               ",\"copies\":[";
        for (size_t c = 0; c < group.copies.size(); c++) {
            const SongCopy& copy = group.copies[c];
            const bool own = copy.package >= 0 && static_cast<size_t>(copy.package) < packages.size();
            const PackageSongs* package = own ? &packages[copy.package] : nullptr;
            if (c) out += ',';
            out += "{\"shortname\":" + JsonString(copy.song.shortname) +
                   ",\"song_id\":" + std::to_string(copy.song.song_id) +
                   ",\"title\":" + JsonString(copy.song.title) +
                   ",\"artist\":" + JsonString(copy.song.artist) +
                   ",\"file\":" + JsonString(package ? package->path : "") +
                   ",\"songs_in_file\":" + std::to_string(package ? package->songs.size() : 0) +
                   ",\"size\":" + std::to_string(package ? package->size : 0) +
                   ",\"in_use\":" + (copy.in_use ? "true" : "false") + "}";
        }
        out += "]}";
    }
    return out + "]}";
}

}
