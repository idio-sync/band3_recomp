#pragma once

// The song's speed (speed.cpp's Game::SetMusicSpeed hook): song_speed's, or
// practice mode's or Rock Band 3 Deluxe's.

namespace band3::speed {

// the speed the current or last song plays at: 1 for a normal song
double MusicSpeed();

}
