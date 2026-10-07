#pragma once
#include "midi_keys.h"

// Whether a song is being played, for the input drivers that play differently
// in a song: a MIDI keyboard's lowest octave is the menu buttons outside one
// (midi_keys.h). src/Hooks/song_pause.cpp works both out from the game and
// sets them here, so Input needn't include Hooks. Any thread.

namespace band3::input {

// a song on a gameplay screen (practice included), as of the game's last read
void SetSongOnScreen(bool on_screen);
// the song is paused, by its pause menu or the game's own
void SetSongPaused(bool paused);

// playing while a song is on a gameplay screen and not paused; menus
// otherwise, and before the game has said either
midi_keys::Mode CurrentKeysMode();

}
