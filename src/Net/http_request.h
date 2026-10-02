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
};

// The request line of `head` (everything up to the blank line); nullopt when
// it isn't "METHOD target HTTP/x".
std::optional<Request> ParseRequest(std::string_view head);

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
};

struct Route {
    Endpoint endpoint = Endpoint::kNotFound;
    int32_t song_id = 0;   // kSong
    std::string argument;  // kJump's and kAlbumArt's shortname, kExecute's script, decoded
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

// not cached unless max_age (seconds) says for how long
std::string Response(int status, std::string_view content_type, std::string_view body,
                     bool cors, int max_age = 0);

}
