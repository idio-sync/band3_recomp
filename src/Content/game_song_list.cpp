#include "game_song_list.h"

#include <algorithm>
#include <cmath>
#include "src/Net/http_request.h"
#include "src/Net/json.h"

namespace band3::content {

namespace {

constexpr int kVersion = 1;

int32_t Whole(const json::Value& v) {
    const double d = v.Number(0);
    return std::isfinite(d) ? int32_t(std::clamp(d, -2147483648.0, 2147483647.0)) : 0;
}

}

std::string FormatGameSongList(const std::vector<ListedSong>& songs) {
    using http::JsonString;
    std::string out = "{\"version\": " + std::to_string(kVersion) + ", \"songs\": [";
    for (size_t i = 0; i < songs.size(); i++) {
        const ListedSong& s = songs[i];
        out += i ? ",\n  " : "\n  ";
        out += "{\"shortname\": " + JsonString(s.shortname) + ", \"song_id\": " + std::to_string(s.song_id) +
               ", \"title\": " + JsonString(s.title) + ", \"artist\": " + JsonString(s.artist) +
               ", \"origin\": " + JsonString(s.origin) + ", \"length_ms\": " + std::to_string(s.length_ms) +
               "}";
    }
    return out + "\n]}\n";
}

std::vector<ListedSong> ParseGameSongList(std::string_view text) {
    std::vector<ListedSong> out;
    const auto json = json::Parse(text);
    if (!json || (*json)["version"].Number(0) != kVersion) return out;
    for (const json::Value& s : (*json)["songs"].Items()) {
        ListedSong song;
        song.shortname = s["shortname"].Text();
        if (song.shortname.empty()) continue;
        song.song_id = Whole(s["song_id"]);
        song.title = s["title"].Text();
        song.artist = s["artist"].Text();
        song.origin = s["origin"].Text();
        song.length_ms = std::max(0, Whole(s["length_ms"]));
        out.push_back(std::move(song));
    }
    return out;
}

std::string OriginName(std::string_view origin) {
    static constexpr std::pair<std::string_view, std::string_view> kNames[] = {
        {"rb1", "Rock Band"},           {"rb1_dlc", "Rock Band DLC"},
        {"rb2", "Rock Band 2"},         {"rb2_dlc", "Rock Band 2 DLC"},
        {"rb3", "Rock Band 3"},         {"rb3_dlc", "Rock Band 3 DLC"},
        {"rb4", "Rock Band 4"},         {"ugc", "Rock Band Network"},
        {"ugc_plus", "Rock Band Network"}, {"ugc1", "Rock Band Network"},
        {"ugc2", "Rock Band Network"},  {"lego", "LEGO Rock Band"},
        {"gdrb", "Green Day: Rock Band"}, {"tbrb", "The Beatles: Rock Band"},
        {"rb4_dlc", "Rock Band 4 DLC"},  {"rbvr", "Rock Band VR"},
        {"fnfestival", "Fortnite Festival"},
    };
    for (const auto& [key, name] : kNames)
        if (key == origin) return std::string(name);
    return std::string(origin);
}

}
