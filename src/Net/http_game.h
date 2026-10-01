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

enum class JumpResult { kJumped, kNotInLibrary, kUnknownSong };

// highlights the song in the Music Library, when the song select panel is up
JumpResult JumpToSong(PPCContext& ctx, uint8_t* base, const std::string& shortname);

// runs DTA through RockCentralGateway::ExecuteConfig, as RB3E's /execute does
void ExecuteScript(PPCContext& ctx, uint8_t* base, const std::string& script);

}
