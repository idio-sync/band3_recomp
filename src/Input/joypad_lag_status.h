#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The extra lag RB3 builds in per controller type, and band3's overrides of it.
// ProfileMgr's constructor fills a table of it once at startup from
// ProfileMgr::GetJoypadExtraLagInits (Hooks/joypad_lag.cpp): a row per
// JoypadType, a column per LagContext, in milliseconds. band3's instruments
// reach the game as Xbox ones, so they get the Xbox hardware's numbers unless
// the joypad_lag setting says otherwise.

namespace band3::input {

// rb3-xenon, src/band3/meta_band/ProfileMgr.h (kNumLagContexts) and .cpp
inline constexpr int kLagContexts = 7;
inline constexpr int kLagJoypadTypes = 47;

// LagContext
inline constexpr uint32_t kLagGame = 0;
inline constexpr uint32_t kLagVideoCalibration = 1;
inline constexpr uint32_t kLagAudioCalibration = 2;

using JoypadLagRow = std::array<float, kLagContexts>;

// One joypad_lag entry: `type=game`, or `type=game/video/audio` with the two
// calibration tests' lag too. A blank part keeps the game's number, so
// `8=/30/` changes only video calibration. The game number also covers the
// practice speeds, which the game gives the same lag.
struct JoypadLagOverride {
    std::optional<float> game;
    std::optional<float> video_calibration;
    std::optional<float> audio_calibration;
};

using JoypadLagOverrides = std::array<std::optional<JoypadLagOverride>, kLagJoypadTypes>;

// Reads joypad_lag's entries, comma separated (e.g. "5=20, 8=30/43/19"), into
// `out`. Returns the entries it couldn't read, to report.
std::vector<std::string> ParseJoypadLagOverrides(std::string_view text, JoypadLagOverrides& out);

// the lag to use for `type` and `context`: the override's, else the game's
float ApplyJoypadLagOverride(const JoypadLagOverrides& overrides, uint32_t type,
                             uint32_t context, float game_ms);

struct JoypadLag {
    // what the game would have used
    JoypadLagRow game{};
    // what it was given
    JoypadLagRow used{};
};

// From the hook, for each entry of the table
void RecordJoypadLag(uint32_t type, uint32_t context, float game_ms, float used_ms);

// the row the game built for `type`, if it built one
std::optional<JoypadLag> JoypadLagFor(uint32_t type);

// LagContext's names, "game", "video calibration", ...
const char* LagContextName(int context);

}
