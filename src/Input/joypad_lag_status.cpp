#include "joypad_lag_status.h"
#include <mutex>

namespace band3::input {

namespace {

std::mutex g_mutex;
std::array<JoypadLagRow, kLagJoypadTypes> g_rows{};
// a bit per context, per type
std::array<uint8_t, kLagJoypadTypes> g_recorded{};
constexpr uint8_t kAllContexts = (1 << kLagContexts) - 1;

}

void RecordJoypadLag(uint32_t type, uint32_t context, float ms) {
    if (type >= kLagJoypadTypes || context >= kLagContexts) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows[type][context] = ms;
    g_recorded[type] |= static_cast<uint8_t>(1 << context);
}

std::optional<JoypadLagRow> JoypadLag(uint32_t type) {
    if (type >= kLagJoypadTypes) return std::nullopt;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_recorded[type] != kAllContexts) return std::nullopt;
    return g_rows[type];
}

const char* LagContextName(int context) {
    switch (context) {
    case 0: return "game";
    case 1: return "video calibration";
    case 2: return "audio calibration";
    case 3: return "practice 90%";
    case 4: return "practice 80%";
    case 5: return "practice 70%";
    case 6: return "practice 60%";
    default: return "?";
    }
}

}
