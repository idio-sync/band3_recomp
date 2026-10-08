#include "live_events.h"
#include <algorithm>
#include "src/Net/http_request.h"

namespace band3::http::live {

namespace {
// RB3's TrackType for vocals (discord.cpp's kTracks lists them in order)
constexpr uint8_t kTrackVocals = 3;
}

LiveState FromSnapshot(const test::GameStateSnapshot& game) {
    LiveState state;
    state.screen = game.screen;
    state.in_game = game.in_game;
    state.paused = game.paused;
    state.shortname = game.song_shortname;
    state.title = game.song_name;
    state.artist = game.song_artist;
    state.length_ms = game.song_length_ms;
    const bool vocalist = std::ranges::any_of(game.band, [](const test::BandMember& m) {
        return m.exists && m.track_type == kTrackVocals;
    });
    state.vocals = !vocalist ? "none" : game.harmonies ? "harmonies" : "lead";
    return state;
}

std::string StreamHead(bool cors) {
    std::string out = "HTTP/1.1 200 OK\r\nServer: band3\r\nContent-Type: text/event-stream\r\n"
                      "Cache-Control: no-store\r\n";
    if (cors) out += "Access-Control-Allow-Origin: *\r\n";
    out += "Connection: close\r\n\r\nretry: 2000\n\n";
    return out;
}

std::string StateEvent(const LiveState& state) {
    auto flag = [](bool b) { return b ? "true" : "false"; };
    std::string out = "event: state\ndata: {\"screen\":" + JsonString(state.screen) +
                      ",\"in_game\":" + flag(state.in_game) + ",\"paused\":" + flag(state.paused) +
                      ",\"song\":";
    if (state.shortname.empty()) {
        out += "null";
    } else {
        out += "{\"shortname\":" + JsonString(state.shortname) +
               ",\"title\":" + JsonString(state.title) + ",\"artist\":" + JsonString(state.artist) +
               ",\"length_ms\":" + std::to_string(state.length_ms) + "}";
    }
    out += ",\"vocals\":" + JsonString(state.vocals) + "}\n\n";
    return out;
}

std::string ClockEvent(int32_t song_ms) {
    return "event: clock\ndata: {\"song_ms\":" + std::to_string(song_ms) + "}\n\n";
}

std::string Ticker::Tick(const LiveState& state, int32_t song_ms, Clock::time_point now) {
    std::string out;
    const bool changed = !sent_ || *sent_ != state;
    if (changed) {
        out += StateEvent(state);
        sent_ = state;
    }
    if (state.in_game && song_ms >= 0 && (changed || !state.paused)) out += ClockEvent(song_ms);
    if (out.empty() && last_sent_ && now - *last_sent_ >= kKeepAliveEvery) {
        out = std::string(kKeepAlive);
    }
    if (!out.empty()) last_sent_ = now;
    return out;
}

}
