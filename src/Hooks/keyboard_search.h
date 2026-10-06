#pragma once
#include <cstdint>

struct PPCContext;

namespace band3::keyboard_search {

// Each frame, on the game thread (frame_counter.cpp's App::DrawRegular): tells
// the window what the keyboard does now (Input/keyboard_search_driver.h) and
// sends the game the next key typed, as RB3Enhanced's KeyboardPoll does.
void RunFrame(PPCContext& ctx, uint8_t* base);

}
