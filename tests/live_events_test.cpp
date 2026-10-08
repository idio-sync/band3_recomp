// Checks what the web server's /live/events sends (src/Net/live_events.cpp).

#include <doctest/doctest.h>
#include <chrono>
#include <string>
#include "src/Net/live_events.h"

using namespace band3::http::live;
using namespace std::chrono_literals;

namespace {

band3::test::GameStateSnapshot Playing() {
    band3::test::GameStateSnapshot game;
    game.screen = "game_screen";
    game.in_game = true;
    game.song_name = "Song";
    game.song_artist = "Artist";
    game.song_shortname = "song";
    game.song_length_ms = 180000;
    game.song_ms = 1000;
    game.band[0] = {true, 3, 1};  // guitar
    return game;
}

}

TEST_CASE("the band's vocals come from its members and the harmonies flag") {
    auto game = Playing();
    CHECK(FromSnapshot(game).vocals == "none");
    game.band[2] = {true, 3, 3};  // vocals
    CHECK(FromSnapshot(game).vocals == "lead");
    game.harmonies = true;
    CHECK(FromSnapshot(game).vocals == "harmonies");
    game.band[2].exists = false;
    CHECK(FromSnapshot(game).vocals == "none");
}

TEST_CASE("the state event carries the song, or null before any") {
    const LiveState state = FromSnapshot(Playing());
    CHECK(StateEvent(state) ==
          "event: state\ndata: {\"screen\":\"game_screen\",\"in_game\":true,\"paused\":false,"
          "\"song\":{\"shortname\":\"song\",\"title\":\"Song\",\"artist\":\"Artist\","
          "\"length_ms\":180000},\"vocals\":\"none\","
          "\"band\":[{\"part\":\"guitar\",\"difficulty\":3},null,null,null]}\n\n");
    CHECK(StateEvent(LiveState{}) ==
          "event: state\ndata: {\"screen\":\"\",\"in_game\":false,\"paused\":false,"
          "\"song\":null,\"vocals\":\"none\",\"band\":[null,null,null,null]}\n\n");
    CHECK(ClockEvent(1234, 0) == "event: clock\ndata: {\"song_ms\":1234,\"score\":0}\n\n");
}

TEST_CASE("the stream's head is an event stream, with CORS when asked for") {
    const std::string head = StreamHead(false);
    CHECK(head.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(head.find("Content-Type: text/event-stream\r\n") != std::string::npos);
    CHECK(head.find("Cache-Control: no-store\r\n") != std::string::npos);
    CHECK(head.find("Content-Length") == std::string::npos);
    CHECK(head.find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(head.ends_with("\r\n\r\nretry: 2000\n\n"));
    CHECK(StreamHead(true).find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
}

TEST_CASE("a ticker sends the state first, then the clock every tick of a song") {
    Ticker ticker;
    const auto t0 = Ticker::Clock::time_point{} + 1h;
    const LiveState state = FromSnapshot(Playing());
    CHECK(ticker.Tick(state, 1000, 0, t0) == StateEvent(state) + ClockEvent(1000, 0));
    CHECK(ticker.Tick(state, 1250, 0, t0 + 250ms) == ClockEvent(1250, 0));
    // a restart takes the clock back; it's sent all the same
    CHECK(ticker.Tick(state, 0, 0, t0 + 500ms) == ClockEvent(0, 0));
}

TEST_CASE("a pause sends the state and the clock once, then keep-alives") {
    Ticker ticker;
    const auto t0 = Ticker::Clock::time_point{} + 1h;
    LiveState state = FromSnapshot(Playing());
    ticker.Tick(state, 1000, 0, t0);
    state.paused = true;
    CHECK(ticker.Tick(state, 1100, 0, t0 + 250ms) == StateEvent(state) + ClockEvent(1100, 0));
    CHECK(ticker.Tick(state, 1100, 0, t0 + 500ms).empty());
    CHECK(ticker.Tick(state, 1100, 0, t0 + 10s).empty());
    CHECK(ticker.Tick(state, 1100, 0, t0 + 250ms + 10s) == kKeepAlive);
    CHECK(ticker.Tick(state, 1100, 0, t0 + 500ms + 10s).empty());
}

TEST_CASE("a fifth stream takes the place of the one open longest") {
    StreamSlots slots(4);
    const uint64_t a = slots.Open(), b = slots.Open(), c = slots.Open(), d = slots.Open();
    CHECK(!slots.Evicted(a));
    CHECK(!slots.Evicted(d));
    const uint64_t e = slots.Open();
    CHECK(slots.Evicted(a));
    CHECK(!slots.Evicted(b));
    CHECK(!slots.Evicted(e));
    // the one it replaced closing changes nothing; another closing makes room
    slots.Close(a);
    slots.Close(c);
    const uint64_t f = slots.Open();
    CHECK(!slots.Evicted(b));
    CHECK(!slots.Evicted(d));
    CHECK(!slots.Evicted(f));
    CHECK(EvictedEvent() == "event: evicted\ndata: {}\n\n");
}

TEST_CASE("no clock goes out while the song's clock isn't known, or outside a song") {
    Ticker ticker;
    const auto t0 = Ticker::Clock::time_point{} + 1h;
    LiveState state = FromSnapshot(Playing());
    CHECK(ticker.Tick(state, -1, 0, t0) == StateEvent(state));
    CHECK(ticker.Tick(state, -1, 0, t0 + 250ms).empty());
    state.in_game = false;
    CHECK(ticker.Tick(state, 5000, 0, t0 + 500ms) == StateEvent(state));
    CHECK(ticker.Tick(state, 5000, 0, t0 + 750ms).empty());
}

TEST_CASE("the band's members carry their part and difficulty, by slot") {
    auto game = Playing();
    game.band[2] = {true, 2, 3};   // vocals, hard
    game.band[3] = {true, 1, 7};   // pro guitar (22-fret), medium
    const LiveState state = FromSnapshot(game);
    REQUIRE(state.band[0]);
    CHECK(state.band[0]->part == "guitar");
    CHECK(state.band[0]->difficulty == 3);
    CHECK(!state.band[1]);
    CHECK(state.band[2]->part == "vocals");
    CHECK(state.band[3]->part == "real_guitar");
    CHECK(state.band[3]->difficulty == 1);
}

TEST_CASE("unknown track types and empty slots are null") {
    auto game = Playing();
    game.band[1] = {true, 3, 10};   // past the track types band3 knows
    game.band[2] = {false, 3, 3};   // not there
    game.band[3] = {true, 9, 4};    // a difficulty past expert: kept as the game said
    const LiveState state = FromSnapshot(game);
    CHECK(!state.band[1]);
    CHECK(!state.band[2]);
    REQUIRE(state.band[3]);
    CHECK(state.band[3]->part == "keys");
    CHECK(StateEvent(state).find("\"band\":[{\"part\":\"guitar\",\"difficulty\":3},null,null,"
                                 "{\"part\":\"keys\",\"difficulty\":9}]") != std::string::npos);
}

TEST_CASE("the clock carries the band's score") {
    CHECK(ClockEvent(1234, 56789) == "event: clock\ndata: {\"song_ms\":1234,\"score\":56789}\n\n");
    Ticker ticker;
    const auto t0 = Ticker::Clock::time_point{} + 1h;
    const LiveState state = FromSnapshot(Playing());
    ticker.Tick(state, 1000, 0, t0);
    CHECK(ticker.Tick(state, 1250, 300, t0 + 250ms) == ClockEvent(1250, 300));
}
