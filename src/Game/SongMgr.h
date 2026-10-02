#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct PPCContext;

// The song library, read through RB3's BandSongMgr (TheSongMgr). These call the
// game's own functions, so they run on the game thread only: from a hook, a
// DTA function, or http::RunGameJobs.

namespace band3::songs {

// what BandSongMetadata says about a song
struct Song {
    int32_t id = 0;
    std::string shortname;
    std::string title;
    std::string artist;
    std::string album;
    std::string origin;  // the game it came from: rb3, rb2, ugc_plus...
    std::string genre;
};

// the song with this ID; nullopt when the song manager doesn't have it
std::optional<Song> Get(PPCContext& ctx, uint8_t* base, int32_t id);

// the IDs of every song the Music Library can show, as
// BandSongMgr::GetRankedSongs lists them (ranked, not private or restricted)
std::vector<int32_t> RankedIds(PPCContext& ctx, uint8_t* base);

// the song's ID, or 0 when no song has this shortname; `symbol` is the
// shortname's Symbol (its interned string's guest address)
int32_t IdFromShortname(PPCContext& ctx, uint8_t* base, uint32_t symbol);

// what the Music Library shows of a song beyond its title and artist
struct Details {
    std::string shortname;
    std::string genre;  // localized, as the Music Library names it
    int32_t year = 0;
    int32_t length_ms = 0;
    int32_t vocal_parts = 0;
    // the difficulty tier (0 Warmup to 6 Impossible) of each part the song has,
    // by the game's name for the part
    std::vector<std::pair<std::string, int32_t>> tiers;
};

// nullopt when the song manager doesn't have the song
std::optional<Details> GetDetails(PPCContext& ctx, uint8_t* base, int32_t id);

// where the song's album art is, as SongMgr::GetAlbumArtPath gives it to the
// Music Library (before the game adds gen/ and _xbox); empty when the song
// has none or no song has this shortname
std::string AlbumArtPath(PPCContext& ctx, uint8_t* base, uint32_t symbol);

}
