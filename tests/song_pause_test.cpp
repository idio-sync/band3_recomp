// Checks how band3's pause menu pauses a song (src/Input/song_pause.cpp): Start
// pressed for each connected player in turn until the song pauses, and again
// for that player to resume it.

#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include "src/Input/song_pause.h"

using namespace band3::input;
using namespace std::chrono_literals;

namespace {

using Clock = SongPause::Clock;
const Clock::time_point t0{};
constexpr auto kHold = SongPause::kHold;
constexpr auto kSettle = SongPause::kSettle;

// one pass of the game's joypad loop: every player read in order, and which
// of them got Start
struct Game {
    std::array<bool, 4> connected{true, false, false, false};
    bool gameplay = true;
    bool paused = false;

    uint32_t Pass(SongPause& pause, Clock::time_point now) const {
        uint32_t pressed = 0;
        for (uint32_t user = 0; user < 4; user++) {
            if (pause.Update(user, connected[user], gameplay, paused, now)) pressed |= 1u << user;
        }
        return pressed;
    }
};

}

TEST_CASE("nothing is pressed until the menu asks") {
    SongPause pause;
    Game game;
    CHECK(game.Pass(pause, t0) == 0);
    CHECK(game.Pass(pause, t0 + 1s) == 0);
}

TEST_CASE("Start is held for the first player until the song pauses") {
    SongPause pause;
    Game game;
    game.Pass(pause, t0);
    pause.Pause();
    CHECK(game.Pass(pause, t0) == 0b0001);
    CHECK(game.Pass(pause, t0 + kHold - 4ms) == 0b0001);
    CHECK(game.Pass(pause, t0 + kHold) == 0);
    game.paused = true;
    CHECK(game.Pass(pause, t0 + kHold + 20ms) == 0);
    // and stays let go
    CHECK(game.Pass(pause, t0 + 2s) == 0);
}

TEST_CASE("a player whose Start doesn't pause the song is passed over for the next") {
    SongPause pause;
    Game game;
    game.connected = {true, false, true, true};
    game.Pass(pause, t0);
    pause.Pause();
    CHECK(game.Pass(pause, t0) == 0b0001);
    CHECK(game.Pass(pause, t0 + kHold) == 0);
    CHECK(game.Pass(pause, t0 + kHold + kSettle - 4ms) == 0);
    // player 1's turn is over; player 3 (the next connected) is pressed next,
    // in the same pass
    const auto second = t0 + kHold + kSettle;
    CHECK(game.Pass(pause, second) == 0b0100);
    CHECK(game.Pass(pause, second + kHold - 4ms) == 0b0100);
    game.paused = true;
    CHECK(game.Pass(pause, second + 50ms) == 0);
}

TEST_CASE("when no player's Start pauses the song, it gives up") {
    SongPause pause;
    Game game;
    game.connected = {true, true, false, false};
    game.Pass(pause, t0);
    pause.Pause();
    auto now = t0;
    uint32_t seen = 0;
    for (int i = 0; i < 1000; i++, now += 4ms) seen |= game.Pass(pause, now);
    CHECK(seen == 0b0011);
    // and then presses nothing more
    CHECK(game.Pass(pause, now) == 0);
    CHECK(game.Pass(pause, now + 1s) == 0);
}

TEST_CASE("a song already paused is left paused, and resuming doesn't touch it") {
    SongPause pause;
    Game game;
    game.paused = true;
    pause.Pause();
    CHECK(game.Pass(pause, t0) == 0);
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 1s) == 0);
    CHECK(game.Pass(pause, t0 + 2s) == 0);
}

TEST_CASE("no Start outside a song, or on its loading and results screens") {
    SongPause pause;
    Game game;
    game.gameplay = false;
    pause.Pause();
    CHECK(game.Pass(pause, t0) == 0);
    // the request is dropped, not kept for when a song starts
    game.gameplay = true;
    CHECK(game.Pass(pause, t0 + 1s) == 0);
}

TEST_CASE("resuming presses Start again for the player who paused it") {
    SongPause pause;
    Game game;
    game.connected = {true, true, false, false};
    game.Pass(pause, t0);
    pause.Pause();
    // player 1's Start doesn't pause it, player 2's does
    game.Pass(pause, t0);
    game.Pass(pause, t0 + kHold + kSettle);
    CHECK(game.Pass(pause, t0 + kHold + kSettle + 4ms) == 0b0010);
    game.paused = true;
    const auto paused_at = t0 + kHold + kSettle + 50ms;
    game.Pass(pause, paused_at);

    const auto resume = paused_at + 5s;
    pause.Resume();
    CHECK(game.Pass(pause, resume) == 0b0010);
    CHECK(game.Pass(pause, resume + kHold - 4ms) == 0b0010);
    CHECK(game.Pass(pause, resume + kHold) == 0);
    game.paused = false;
    CHECK(game.Pass(pause, resume + 1s) == 0);
}

TEST_CASE("a resume asked for while the pause is still pressing waits for it, with a gap") {
    SongPause pause;
    Game game;
    game.Pass(pause, t0);
    pause.Pause();
    CHECK(game.Pass(pause, t0) == 0b0001);
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 20ms) == 0b0001);
    game.paused = true;
    const auto paused_at = t0 + 40ms;
    CHECK(game.Pass(pause, paused_at) == 0);
    // let go for a hold before pressing again, so the game sees two presses
    CHECK(game.Pass(pause, paused_at + kHold - 4ms) == 0);
    CHECK(game.Pass(pause, paused_at + kHold) == 0b0001);
    CHECK(game.Pass(pause, paused_at + kHold * 2) == 0);
}

TEST_CASE("pausing again before a resume starts keeps the song paused") {
    SongPause pause;
    Game game;
    game.Pass(pause, t0);
    pause.Pause();
    game.Pass(pause, t0);
    game.paused = true;
    game.Pass(pause, t0 + 20ms);
    pause.Resume();
    pause.Pause();
    CHECK(game.Pass(pause, t0 + 1s) == 0);
    // and it still knows who paused it
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 2s) == 0b0001);
}

TEST_CASE("a song unpaused some other way isn't resumed again, nor one that ended") {
    SongPause pause;
    Game game;
    game.Pass(pause, t0);
    pause.Pause();
    game.Pass(pause, t0);
    game.paused = true;
    game.Pass(pause, t0 + 20ms);
    game.paused = false;
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 1s) == 0);

    pause.Pause();
    game.Pass(pause, t0 + 2s);
    game.paused = true;
    game.Pass(pause, t0 + 2s + 20ms);
    game.gameplay = false;
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 3s) == 0);
}

TEST_CASE("forgetting the pause leaves the song paused") {
    SongPause pause;
    Game game;
    game.Pass(pause, t0);
    pause.Pause();
    game.Pass(pause, t0);
    game.paused = true;
    game.Pass(pause, t0 + 20ms);
    pause.Forget();
    pause.Resume();
    CHECK(game.Pass(pause, t0 + 1s) == 0);
}
