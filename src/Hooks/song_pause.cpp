#include "song_pause.h"
#include <rex/hook.h>
#include <rex/input/input.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <array>
#include <atomic>
#include <mutex>
#include <string_view>
#include "generated/band3_init.h"
#include "src/Input/instruments.h"
#include "src/Input/song_pause.h"
#include "src/Render/native_view.h"

// Whether the song is paused comes from Game::UpdatePausedState, which every
// pause and unpause goes through: the song's pause menu (the overshell's) and
// the game's own (set_paused) alike. Which screen is up comes from BandUI, as
// rb3e_events.cpp reads it. Offsets are TU5's (rb3-xenon game/Game.h for the
// members).

extern "C" void __imp__Game__UpdatePausedState(PPCContext& ctx, uint8_t* base);

namespace band3::song_pause {

namespace {

// RB3Enhanced's Xbox 360 TU5 addresses and offsets, as rb3e_events.cpp's
constexpr uint32_t kTheBandUI = 0x82DFD2B0;       // BandUI object
constexpr uint32_t kBandUI_CurrentScreen = 0x2C;  // UIScreen*
constexpr uint32_t kUIScreen_Name = 0x18;         // Symbol (char*)
// Game::mIsPaused, before mGameWantsPause (+0x79) and mOvershellWantsPause
// (+0x7A), which UpdatePausedState ORs into it
constexpr uint32_t kGame_IsPaused = 0x78;

std::atomic<bool> g_paused{false};

struct Shared {
    std::mutex mutex;
    input::SongPause pause;
    // each player's: whether Start was added last time, and how far the
    // packet numbers have been moved
    std::array<bool, input::SongPause::kPlayers> added{};
    std::array<uint32_t, input::SongPause::kPlayers> packets{};
};

Shared& State() {
    static Shared shared;
    return shared;
}

uint32_t Load32(uint8_t* base, uint32_t addr) {
    return *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, addr);
}

// A song being played, where Start pauses it: game_screen, practice_game_screen
// and the trainers' like them, but not the results (coop_endgame_screen), a
// restart's screen, or Deluxe's loading screens before a song
// (mydta_start_in_game_screen)
bool GameplayScreen(uint8_t* base) {
    const uint32_t screen = Load32(base, kTheBandUI + kBandUI_CurrentScreen);
    const uint32_t name = screen ? Load32(base, screen + kUIScreen_Name) : 0;
    if (!name) return false;
    const std::string_view text = rex::memory::GuestPtr<const char*>(base, name);
    return text.ends_with("game_screen") && text.find("endgame") == std::string_view::npos &&
           text.find("restart") == std::string_view::npos && !text.ends_with("in_game_screen");
}

}

void Pause() {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().pause.Pause();
}

void Resume() {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().pause.Resume();
}

void Forget() {
    std::lock_guard<std::mutex> lock(State().mutex);
    State().pause.Forget();
}

void ResetPaused() { g_paused.store(false, std::memory_order_relaxed); }

void AddPress(uint32_t user, rex::input::X_INPUT_STATE* state, uint8_t* base) {
    if (user >= input::SongPause::kPlayers) return;
    const bool gameplay = render::InSong() && GameplayScreen(base);
    Shared& shared = State();
    std::lock_guard<std::mutex> lock(shared.mutex);
    const bool press = shared.pause.Update(user, state != nullptr, gameplay,
                                           g_paused.load(std::memory_order_relaxed),
                                           input::SongPause::Clock::now());
    if (!state) return;
    if (press != shared.added[user]) {
        shared.added[user] = press;
        shared.packets[user]++;
    }
    if (press) state->gamepad.buttons = static_cast<uint16_t>(state->gamepad.buttons | input::xbox::kStart);
    state->packet_number = state->packet_number + shared.packets[user];
}

}

// Game::UpdatePausedState(Game*, bool, bool)
extern "C" REX_FUNC(Game__UpdatePausedState) {
    const uint32_t game = ctx.r3.u32;
    __imp__Game__UpdatePausedState(ctx, base);
    if (!game) return;
    const bool paused = *rex::memory::GuestPtr<uint8_t*>(base, game + band3::song_pause::kGame_IsPaused) != 0;
    if (band3::song_pause::g_paused.exchange(paused, std::memory_order_relaxed) != paused) {
        REXLOG_INFO("Song: {}", paused ? "paused" : "resumed");
    }
}
