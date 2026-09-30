#pragma once
#include <array>
#include <cstdint>
#include <optional>

// The extra lag RB3 builds in per controller type, for the Instrument Lab.
// ProfileMgr's constructor fills a table of it once at startup from
// ProfileMgr::GetJoypadExtraLagInits (Hooks/joypad_lag.cpp): a row per
// JoypadType, a column per LagContext, in milliseconds. band3's instruments
// reach the game as Xbox ones, so they get the Xbox hardware's numbers.

namespace band3::input {

// rb3-xenon, src/band3/meta_band/ProfileMgr.h (kNumLagContexts) and .cpp
inline constexpr int kLagContexts = 7;
inline constexpr int kLagJoypadTypes = 47;

using JoypadLagRow = std::array<float, kLagContexts>;

// From the hook, for each entry of the table
void RecordJoypadLag(uint32_t type, uint32_t context, float ms);

// the row the game built for `type`, if it built one
std::optional<JoypadLagRow> JoypadLag(uint32_t type);

// LagContext's names, "game", "video calibration", ...
const char* LagContextName(int context);

}
