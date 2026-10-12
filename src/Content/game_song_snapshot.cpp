// The game's own song list, kept for the launcher (game_song_list.h): a
// thread asks the game for its songs' IDs now and then, and when they've
// changed reads the songs, a chunk a frame as /song_details does, so a
// library of thousands can't hold up a frame, and writes them out.

#include "game_song_list.h"

#include <rex/filesystem.h>
#include <rex/logging.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <thread>
#include <unordered_map>

#include "src/Net/http_game.h"
#include "src/Net/http_server.h"
#include "src/config.h"

namespace band3::content {

namespace {

namespace fs = std::filesystem;

constexpr std::string_view kFile = "band3_game_songs.json";
// how often the song IDs are looked at; songs are added in the Music
// Library, and by downloads
constexpr auto kEvery = std::chrono::seconds(30);
constexpr size_t kChunk = 100;

fs::path FilePath() {
    const fs::path root = UserDataRoot();
    return root.empty() ? fs::path() : root / kFile;
}

void Write(const std::vector<ListedSong>& songs) {
    const fs::path path = FilePath();
    if (path.empty()) return;
    fs::path temp = path;
    temp += ".partial";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        out << FormatGameSongList(songs);
        if (!out) {
            REXLOG_WARN("Game songs: couldn't write {}", rex::path_to_utf8(temp));
            return;
        }
    }
    std::error_code ec;
    fs::rename(temp, path, ec);
    if (ec) REXLOG_WARN("Game songs: couldn't write {}: {}", rex::path_to_utf8(path), ec.message());
}

// the songs of these IDs, or none if the game was busy
std::optional<std::vector<ListedSong>> Read(const std::vector<int32_t>& ids) {
    std::vector<ListedSong> songs;
    songs.reserve(ids.size());
    for (size_t at = 0; at < ids.size(); at += kChunk) {
        const std::vector<int32_t> chunk(ids.begin() + at,
                                         ids.begin() + std::min(at + kChunk, ids.size()));
        if (!http::RunOnGameThreadAndWait([&songs, &chunk](PPCContext& ctx, uint8_t* base) {
                std::unordered_map<std::string, int32_t> lengths;
                for (const http::SongDetails& d : http::game::Details(ctx, base, chunk))
                    lengths[d.shortname] = d.length_ms;
                for (const int32_t id : chunk) {
                    const auto song = http::game::Song(ctx, base, id);
                    if (!song || song->shortname.empty()) continue;
                    const auto length = lengths.find(song->shortname);
                    songs.push_back({song->shortname, id, song->title, song->artist, song->origin,
                                     length == lengths.end() ? 0 : length->second});
                }
            })) {
            return std::nullopt;
        }
    }
    return songs;
}

void Run() {
    std::vector<int32_t> last;
    for (;;) {
        std::this_thread::sleep_for(kEvery);
        std::vector<int32_t> ids;
        if (!http::RunOnGameThreadAndWait(
                [&ids](PPCContext& ctx, uint8_t* base) { ids = http::game::RankedIds(ctx, base); }))
            continue;
        // none yet (still booting), or the same as last written
        if (ids.empty() || ids == last) continue;
        std::optional<std::vector<ListedSong>> songs = Read(ids);
        if (!songs) continue;
        Write(*songs);
        REXLOG_INFO("Game songs: {} listed, kept for the launcher", songs->size());
        last = std::move(ids);
    }
}

}

std::vector<ListedSong> LastGameSongs() {
    std::ifstream in(FilePath(), std::ios::binary);
    if (!in) return {};
    std::stringstream text;
    text << in.rdbuf();
    return ParseGameSongList(text.str());
}

void StartGameSongSnapshot() {
    // leaked: it lives as long as the game
    std::thread(Run).detach();
}

}
