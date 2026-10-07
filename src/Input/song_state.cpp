#include "song_state.h"
#include <atomic>

namespace band3::input {

namespace {

std::atomic<bool> g_on_screen{false};
std::atomic<bool> g_paused{false};

}

void SetSongOnScreen(bool on_screen) { g_on_screen.store(on_screen, std::memory_order_relaxed); }

void SetSongPaused(bool paused) { g_paused.store(paused, std::memory_order_relaxed); }

midi_keys::Mode CurrentKeysMode() {
    return g_on_screen.load(std::memory_order_relaxed) && !g_paused.load(std::memory_order_relaxed)
               ? midi_keys::Mode::kPlaying
               : midi_keys::Mode::kMenus;
}

}
