#pragma once
#include <chrono>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include "src/Test/game_state.h"

// The web server's /live/events (http_server.h): what the game is doing, as
// server-sent events, for the pages other devices show (/karaoke, and those
// after it). The server's thread for each stream writes what Ticker gives it;
// nothing here touches a socket or the game.
//
//   event: state  {"screen":..., "in_game":..., "paused":..., "song": {"shortname":...,
//                  "title":..., "artist":..., "length_ms":...} or null,
//                  "vocals": "none" | "lead" | "harmonies"}   when any of it changes
//   event: clock  {"song_ms": ...}   every tick of a song not paused, and with each state

namespace band3::http::live {

struct LiveState {
    std::string screen;
    bool in_game = false;
    bool paused = false;
    // the last song entered, kept after it ends; empty before any
    std::string shortname;
    std::string title;
    std::string artist;
    int32_t length_ms = 0;
    std::string vocals = "none";
    bool operator==(const LiveState&) const = default;
};

LiveState FromSnapshot(const test::GameStateSnapshot& game);

// the reply's head (200, text/event-stream, not cached), then the delay a
// browser waits before reconnecting
std::string StreamHead(bool cors);
std::string StateEvent(const LiveState& state);
std::string ClockEvent(int32_t song_ms);
// a comment, which only keeps the connection alive
inline constexpr std::string_view kKeepAlive = ": keep-alive\n\n";
// the last event of a stream another took the place of (StreamSlots): its
// page stops reconnecting by itself
std::string EvictedEvent();

// Which streams go on: at most `max`, and a new one takes the place of the
// one open longest, since a device asleep can hold its connection open for
// hours while the newcomer is the page being watched. Not thread-safe: the
// server keeps it under its lock.
class StreamSlots {
public:
    explicit StreamSlots(size_t max) : max_(max) {}
    // a new stream's id
    uint64_t Open();
    // whether another stream took this one's place
    bool Evicted(uint64_t id) const;
    void Close(uint64_t id);

private:
    size_t max_;
    uint64_t next_ = 1;
    std::deque<uint64_t> open_;  // oldest first; the evicted aren't here
};

// What one stream sends at each tick (the server's loop, every 250 ms): the
// state the first time and whenever it changes; the clock with every state,
// and at every tick of a song that isn't paused, while the song's clock is
// known; a keep-alive after kKeepAliveEvery of nothing.
class Ticker {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr auto kKeepAliveEvery = std::chrono::seconds(10);

    std::string Tick(const LiveState& state, int32_t song_ms, Clock::time_point now);

private:
    std::optional<LiveState> sent_;
    std::optional<Clock::time_point> last_sent_;
};

}
