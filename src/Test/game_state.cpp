#include "game_state.h"
#include <utility>

namespace band3::test {

GameState& GameState::Get() {
    static GameState state;
    return state;
}

void GameState::SetScreen(std::string screen) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.screen = std::move(screen);
}

void GameState::SetInGame(bool in_game) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.in_game = in_game;
}

void GameState::SetSong(std::string name, std::string artist, std::string shortname) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.song_name = std::move(name);
    state_.song_artist = std::move(artist);
    state_.song_shortname = std::move(shortname);
}

void GameState::SetVenue(std::string venue) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.venue = std::move(venue);
}

void GameState::SetBand(const std::array<BandMember, 4>& band) {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.band = band;
}

void GameState::CountFrame() {
    std::lock_guard<std::mutex> lock(mutex_);
    state_.frame++;
}

GameStateSnapshot GameState::Snapshot() {
    std::lock_guard<std::mutex> lock(mutex_);
    return state_;
}

}
