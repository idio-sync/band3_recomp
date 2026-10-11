#pragma once
#include <cstdint>

struct PPCContext;

// How full the game's heaps are, logged as each song starts and ends (Game's
// constructor and destructor, rb3e_events.cpp), and what ran out when the game
// can't allocate (src/Game/heap_report.h has the lines).

namespace band3::heap_report {

// one line with every heap's free space; `when` says what the game is doing
void LogHeaps(PPCContext& ctx, uint8_t* base, const char* when);

}
