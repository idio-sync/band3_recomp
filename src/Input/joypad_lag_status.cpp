#include "joypad_lag_status.h"
#include <charconv>
#include <cmath>
#include <mutex>

namespace band3::input {

namespace {

std::mutex g_mutex;
std::array<JoypadLag, kLagJoypadTypes> g_rows{};
// a bit per context, per type
std::array<uint8_t, kLagJoypadTypes> g_recorded{};
constexpr uint8_t kAllContexts = (1 << kLagContexts) - 1;

// more than any controller's lag; a bigger number is a typo
constexpr float kMaxLagMs = 1000.0f;

std::string_view Trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

// a blank part is a kept number; false for one that isn't a number
bool ParsePart(std::string_view part, std::optional<float>& out) {
    part = Trim(part);
    if (part.empty()) return true;
    float ms = 0;
    auto [end, ec] = std::from_chars(part.data(), part.data() + part.size(), ms);
    if (ec != std::errc() || end != part.data() + part.size()) return false;
    if (!std::isfinite(ms) || std::fabs(ms) > kMaxLagMs) return false;
    out = ms;
    return true;
}

std::optional<JoypadLagOverride> ParseValues(std::string_view values) {
    std::optional<float> parts[3];
    int count = 0;
    while (true) {
        if (count == 3) return std::nullopt;
        const size_t slash = values.find('/');
        if (!ParsePart(values.substr(0, slash), parts[count++])) return std::nullopt;
        if (slash == std::string_view::npos) break;
        values.remove_prefix(slash + 1);
    }
    // `5=` or `5=//` changes nothing, so it's more likely a mistake
    if (!parts[0] && !parts[1] && !parts[2]) return std::nullopt;
    return JoypadLagOverride{parts[0], parts[1], parts[2]};
}

}

std::vector<std::string> ParseJoypadLagOverrides(std::string_view text, JoypadLagOverrides& out) {
    std::vector<std::string> bad;
    while (!text.empty()) {
        const size_t comma = text.find(',');
        const std::string_view entry = Trim(text.substr(0, comma));
        text.remove_prefix(comma == std::string_view::npos ? text.size() : comma + 1);
        if (entry.empty()) continue;

        const size_t equals = entry.find('=');
        unsigned type = 0;
        std::optional<JoypadLagOverride> values;
        if (equals != std::string_view::npos) {
            const std::string_view number = Trim(entry.substr(0, equals));
            auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), type);
            if (ec == std::errc() && end == number.data() + number.size() &&
                type < kLagJoypadTypes) {
                values = ParseValues(entry.substr(equals + 1));
            }
        }
        if (!values) {
            bad.emplace_back(entry);
            continue;
        }
        out[type] = *values;
    }
    return bad;
}

float ApplyJoypadLagOverride(const JoypadLagOverrides& overrides, uint32_t type,
                             uint32_t context, float game_ms) {
    if (type >= kLagJoypadTypes || !overrides[type]) return game_ms;
    const JoypadLagOverride& o = *overrides[type];
    const std::optional<float>& ms = context == kLagVideoCalibration   ? o.video_calibration
                                     : context == kLagAudioCalibration ? o.audio_calibration
                                                                       : o.game;
    return ms.value_or(game_ms);
}

void RecordJoypadLag(uint32_t type, uint32_t context, float game_ms, float used_ms) {
    if (type >= kLagJoypadTypes || context >= kLagContexts) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    g_rows[type].game[context] = game_ms;
    g_rows[type].used[context] = used_ms;
    g_recorded[type] |= static_cast<uint8_t>(1 << context);
}

std::optional<JoypadLag> JoypadLagFor(uint32_t type) {
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
