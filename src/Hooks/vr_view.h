#pragma once

#include <cstdint>

struct PPCContext;

// VR's hooks' work outside the hooked calls (src/Hooks/vr_view.cpp)

namespace band3::vr_view {

// Once a frame, before the game draws it (frame_counter.cpp's App::DrawRegular):
// whether a fixed viewpoint (vr_viewpoint front_row, on_stage, venue, with vr
// on) is seen in a song, so every shot draws the whole world, and as it
// starts, what the shot hid shown at once
void RunFrame(PPCContext& ctx, uint8_t* base);

}
