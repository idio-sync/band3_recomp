#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// The songs the game itself has (its Music Library: the disc's, exports and
// DLC as well as band3's packages), as it last listed them, kept in the user
// data root as band3_game_songs.json so the launcher knows them before the
// game boots (game_song_snapshot.cpp writes it while the game runs):
// {"version": 1, "songs": [{"shortname":, "song_id":, "title":, "artist":,
// "origin":, "length_ms":}]}.

namespace band3::content {

struct ListedSong {
    std::string shortname;
    int32_t song_id = 0;
    std::string title;
    std::string artist;
    std::string origin;  // the game it came from, as the game says it (rb3, rb1, ugc_plus...)
    int32_t length_ms = 0;
};

std::string FormatGameSongList(const std::vector<ListedSong>& songs);
// what can't be read is left out; another version's file gives none
std::vector<ListedSong> ParseGameSongList(std::string_view json);

// an origin as the Music Videos screen names it: "Rock Band 3", "Rock Band 2"...; the
// origin itself for one it doesn't know
std::string OriginName(std::string_view origin);

// The game's songs as last listed (the snapshot file); none before the game
// has run with them. Any thread.
std::vector<ListedSong> LastGameSongs();

// Lists the game's songs now and then while it runs, writing the snapshot
// when they change (game_song_snapshot.cpp). Call once, as the game starts.
void StartGameSongSnapshot();

}
