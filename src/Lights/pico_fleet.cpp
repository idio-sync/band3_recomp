#include "pico_fleet.h"
#include <cmath>
#include "src/Net/json.h"

namespace band3::lights {

std::array<uint8_t, 10> StageKitPacket(Command command) {
    // RB3E_EventHeader, as band3's own events send it (src/Net/events.cpp)
    return {'R', 'B', '3', 'E', 0, 6, 2, 0xFF, command.left, command.right};
}

std::optional<PicoStatus> ParseTelemetry(std::string_view text) {
    const auto value = json::Parse(text);
    if (!value || !value->IsObject()) return std::nullopt;
    const json::Value& name = (*value)["name"];
    const json::Value& usb = (*value)["usb_status"];
    const json::Value& signal = (*value)["wifi_signal"];
    // the dashboard takes only these, with an integer signal
    if (!name.IsString() || !usb.IsString() || !signal.IsNumber()) return std::nullopt;
    const double dbm = signal.Number();
    if (dbm != std::floor(dbm) || std::abs(dbm) > 1000) return std::nullopt;
    PicoStatus status;
    if ((*value)["id"].IsString()) status.id = (*value)["id"].Text();
    status.name = name.Text();
    status.usb_status = usb.Text();
    status.wifi_signal = static_cast<int>(dbm);
    return status;
}

std::string FormatAddress(uint32_t address) {
    std::string text;
    for (int i = 0; i < 4; i++) {
        if (i) text += '.';
        text += std::to_string((address >> (8 * i)) & 0xFF);
    }
    return text;
}

void PicoFleet::Heard(uint32_t address, PicoStatus status, Clock::time_point now) {
    picos_[address] = {std::move(status), now};
}

std::vector<PicoFleet::Pico> PicoFleet::List(Clock::time_point now) {
    std::vector<Pico> list;
    for (auto it = picos_.begin(); it != picos_.end();) {
        const auto quiet = now - it->second.heard;
        if (quiet >= kForgotten) {
            it = picos_.erase(it);
            continue;
        }
        list.push_back({it->first, it->second.status, quiet < kOffline});
        ++it;
    }
    return list;
}

}  // namespace band3::lights
