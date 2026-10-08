#pragma once

#include <cstdint>

// The music video hooks' work outside a movie's draw (src/Hooks/music_video.cpp)

namespace band3::music_video {

// Once a frame, before the game draws it (frame_counter.cpp's App::DrawRegular):
// with music_video_as_is on, TheRnd's post-processing off while a song's video
// (or the black background's black) shows, and back on after
void RunFrame(uint8_t* base);

}
