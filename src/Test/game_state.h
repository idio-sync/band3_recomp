#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

// What the test harness knows about the game, kept up to date by the hooks that
// already follow it for the RB3E events (src/Hooks/rb3e_events.cpp), and read
// by the test server's thread.

namespace band3::test {

struct BandMember {
    bool exists = false;
    uint8_t difficulty = 0;
    uint8_t track_type = 0;
};

// one of band3's USB mic slots (src/Audio/usb_mic_capture.h)
struct MicSlot {
    // what records it; empty while nothing does
    std::string device;
    // the game took the connection
    bool connected = false;
    // audio handed to the game
    uint64_t bytes_fed = 0;
};

struct GameStateSnapshot {
    std::string screen;
    bool in_game = false;
    // the last song entered, kept after it ends
    std::string song_name;
    std::string song_artist;
    std::string song_shortname;
    std::string venue;
    std::array<BandMember, 4> band{};
    // frames the game has drawn since it started
    uint64_t frame = 0;
    // the band's score on the song's scoreboard; 0 when a song starts, and
    // kept after it ends
    int64_t score = 0;
    // the USB mic slots, while usb_mics records; the hooks don't keep these,
    // the test server adds them
    std::vector<MicSlot> mics;
};

class GameState {
public:
    // the game's, which the hooks update
    static GameState& Get();

    void SetScreen(std::string screen);
    void SetInGame(bool in_game);
    void SetSong(std::string name, std::string artist, std::string shortname);
    void SetVenue(std::string venue);
    void SetBand(const std::array<BandMember, 4>& band);
    void SetScore(int64_t score);
    void CountFrame();

    GameStateSnapshot Snapshot();

private:
    std::mutex mutex_;
    GameStateSnapshot state_;
};

}
