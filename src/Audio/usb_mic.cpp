#include "usb_mic.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <numbers>

namespace band3::audio::usb_mic {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

char Lower(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

}

std::vector<std::string> ParseDeviceList(std::string_view list) {
    std::vector<std::string> names;
    if (Trim(list).empty()) return names;
    while (true) {
        const size_t comma = list.find(',');
        names.emplace_back(Trim(list.substr(0, comma)));
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return names;
}

bool NameMatches(std::string_view device, std::string_view wanted) {
    if (wanted.empty()) return false;
    const auto it = std::ranges::search(device, wanted, {}, Lower, Lower);
    return !it.empty();
}

ToneSource::ToneSource(double hz, Clock::time_point start, int16_t amplitude)
    : step_(2 * std::numbers::pi * hz / kSampleRate), amplitude_(amplitude), start_(start) {}

size_t ToneSource::Read(std::span<uint8_t> out, Clock::time_point now) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(now - start_);
    if (elapsed.count() <= 0) return 0;
    const uint64_t due = static_cast<uint64_t>(elapsed.count()) * kSampleRate / 1'000'000;
    if (due <= produced_) return 0;

    constexpr uint64_t kMaxOwed = kMaxBacklogBytes / 2;
    if (due - produced_ > kMaxOwed) produced_ = due - kMaxOwed;

    const size_t count = static_cast<size_t>(std::min<uint64_t>(due - produced_, out.size() / 2));
    for (size_t i = 0; i < count; i++) {
        const auto sample = static_cast<int16_t>(std::lround(amplitude_ * std::sin(phase_)));
        const auto bits = static_cast<uint16_t>(sample);
        out[i * 2] = static_cast<uint8_t>(bits >> 8);
        out[i * 2 + 1] = static_cast<uint8_t>(bits);
        phase_ += step_;
        if (phase_ >= 2 * std::numbers::pi) phase_ -= 2 * std::numbers::pi;
    }
    produced_ += count;
    return count * 2;
}

Action Slot::Next(bool source_ready, Clock::time_point now) const {
    if (connected_) return source_ready ? Action::kFeed : Action::kDisconnect;
    if (source_ready && now >= retry_) return Action::kConnect;
    return Action::kWait;
}

void Slot::Connected(bool accepted, Clock::time_point now) {
    connected_ = accepted;
    retry_ = accepted ? Clock::time_point{} : now + kConnectRetry;
}

void Slot::Disconnected() {
    connected_ = false;
    retry_ = {};
}

}
