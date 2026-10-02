#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The parts of the web server (http_server.h) that don't need a socket or the
// game: reading a request, picking its endpoint and writing the replies, in
// RB3Enhanced's formats (source/net_http_server.c in its repo), so its web page
// and the tools written for it work unchanged.

namespace band3::http {

struct Request {
    std::string method;
    // as sent, still percent-encoded
    std::string target;
    // the headers a POST needs; empty and 0 when not sent
    std::string content_type;
    size_t content_length = 0;
    std::string body;  // the server reads it in after the head
};

// The request line and headers of `head` (everything up to the blank line);
// nullopt when the line isn't "METHOD target HTTP/x" or Content-Length isn't
// a number.
std::optional<Request> ParseRequest(std::string_view head);

// a query parameter of a target (?a=1&b=2), decoded, with '+' as a space as
// forms send it; nullopt when it isn't there
std::optional<std::string> QueryParam(std::string_view target, std::string_view name);

// %XX escapes decoded; '+' stays '+', as RB3E leaves it, since DTA scripts use it
std::string UrlDecode(std::string_view text);

enum class Endpoint {
    kNotFound,
    kIndex,      // /                  the web page
    kSong,       // /song_<id>         one song's details
    kListSongs,  // /list_songs        every song in the library
    kJump,       // /jump?shortname=   select a song in the Music Library
    kExecute,    // /execute?script=   run a DTA script (http_allow_scripts)
    kJsonRpc,    // /jsonrpc           discordrp.json from the game data root
    kAlbumArt,   // /album_art?shortname=  a song's album art (band3's, not RB3E's)
    kStatus,     // /status            what the game is doing (band3's)
    kSongDetails,  // /song_details    every song's genre, year, parts... (band3's)
    // RhythmVerse (band3's; src/Net/rhythmverse.h)
    kRvSearch,     // /rv/search?text=&page=  a page of RhythmVerse's songs
    kRvDownload,   // POST /rv/download {"file_id": ...}  download one to the songs folder
    kRvDownloads,  // /rv/downloads    the downloads, and how far along they are
};

struct Route {
    Endpoint endpoint = Endpoint::kNotFound;
    int32_t song_id = 0;   // kSong
    // kJump's and kAlbumArt's shortname, kExecute's script, decoded
    std::string argument;
};

// RB3E matches the decoded target, so /jump?shortname=a%26b jumps to "a&b"
Route MatchRoute(std::string_view target);

// one song, as /song_<id> and /list_songs report it
struct SongInfo {
    std::string shortname;
    std::string title;
    std::string artist;
    std::string album;
    std::string origin;
};

// RB3E's INI-style block: shortname=, title=, artist=, album=, origin=, each on
// a line of its own, then a blank line. /list_songs puts a [shortname] line
// before each.
std::string FormatSong(const SongInfo& song, bool section);

// The game's strings are UTF-8 for songs that say so and Latin-1 for the
// rest; anything that isn't valid UTF-8 is taken as Latin-1, so the replies
// can all be UTF-8.
std::string ToUtf8(std::string_view text);

// a JSON string, quoted, of the game's text (made UTF-8 as ToUtf8 does)
std::string JsonString(std::string_view text);

// what /status reports, for the page's banner
struct Status {
    struct Playing {
        std::string shortname;
        std::string title;
        std::string artist;
        int64_t score = 0;
        int32_t position_ms = -1;  // -1 until the song's clock is known
        int32_t length_ms = 0;
    };
    std::string screen;
    // the Music Library is up, so /jump can select a song
    bool in_library = false;
    std::optional<Playing> playing;  // during a song
};

// {"screen":..., "in_library":..., "playing": {...} or null}
std::string FormatStatus(const Status& status);

// what /song_details says of a song, beyond /list_songs
struct SongDetails {
    std::string shortname;
    std::string genre;  // as the Music Library names it
    int32_t year = 0;
    int32_t length_ms = 0;
    int32_t vocal_parts = 0;
    // the game's difficulty tier, 0 (Warmup) to 6 (Impossible), of each part the
    // song has, by the game's name for it: band, guitar, bass, drum, vocals,
    // keys, real_guitar, real_bass, real_keys
    std::vector<std::pair<std::string, int32_t>> tiers;
};

// {"<shortname>": {"genre":..., "year":..., "length_ms":..., "vocal_parts":...,
// "tiers": {"<part>": tier, ...}}, ...}
std::string FormatSongDetails(const std::vector<SongDetails>& songs);

// not cached unless max_age (seconds) says for how long
std::string Response(int status, std::string_view content_type, std::string_view body,
                     bool cors, int max_age = 0);

}
