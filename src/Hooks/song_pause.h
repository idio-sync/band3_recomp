#pragma once
#include <cstdint>

namespace rex::input {
struct X_INPUT_STATE;
}

// The song under band3's pause menu, paused and resumed with Start pressed for
// the game (src/Input/song_pause.h has how).

namespace band3::song_pause {

// The pause menu, on the UI thread: pause the song if one is on and isn't
// paused; resume it if Pause paused it; or leave it as it is from now on.
void Pause();
void Resume();
void Forget();

// From the game's XInputGetState (src/Hooks/input_lock.cpp), after each read:
// `state` is what the game read for `user`, or null when the player has no
// controller. Start joins its buttons while it's pressed for the player.
void AddPress(uint32_t user, rex::input::X_INPUT_STATE* state, uint8_t* base);

// Game's constructor and destructor (rb3e_events.cpp): a new song, or none,
// isn't paused
void ResetPaused();

}
