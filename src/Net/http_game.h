#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "http_request.h"

struct PPCContext;

// What the web server (http_server.h) asks of the game. These call RB3's own
// functions, so they run on the game thread only, from http::RunGameJobs.

namespace band3::http::game {

// the song with this ID; nullopt when the song manager doesn't have it
std::optional<SongInfo> Song(PPCContext& ctx, uint8_t* base, int32_t id);

// every song the Music Library can show, as BandSongMgr::GetRankedSongs lists them
std::vector<SongInfo> RankedSongs(PPCContext& ctx, uint8_t* base);

// the same songs' IDs, cheaper than RankedSongs, to see if the list changed
std::vector<int32_t> RankedIds(PPCContext& ctx, uint8_t* base);

// what /song_details says of these songs (IDs from RankedIds); a song the
// song manager doesn't have any more is left out
std::vector<SongDetails> Details(PPCContext& ctx, uint8_t* base,
                                 const std::vector<int32_t>& ids);

enum class JumpResult { kJumped, kNotInLibrary, kUnknownSong };

// highlights the song in the Music Library, when the song select panel is up
JumpResult JumpToSong(PPCContext& ctx, uint8_t* base, const std::string& shortname);

// the song's album art as the game reads it for the Music Library (its
// .png_xbox, album_art.h decodes it); nullopt when the song has none or no
// song has this shortname
std::optional<std::string> AlbumArtFile(PPCContext& ctx, uint8_t* base,
                                        const std::string& shortname);

// the song's MIDI file as the game reads it, for /lyrics (lyrics.h);
// nullopt when no song has this shortname or the game can't read it
std::optional<std::string> MidiFile(PPCContext& ctx, uint8_t* base, const std::string& shortname);

// a file as the game reads it (ARK, loose overrides, packages), for
// /game_asset; nullopt when the game can't open it or it's over max_size
std::optional<std::string> GameFile(PPCContext& ctx, uint8_t* base, const std::string& path,
                                    size_t max_size);

// runs DTA through RockCentralGateway::ExecuteConfig, as RB3E's /execute does
void ExecuteScript(PPCContext& ctx, uint8_t* base, const std::string& script);

}
