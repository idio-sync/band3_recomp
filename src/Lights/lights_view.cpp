#include "lights_view.h"
#include <charconv>
#include <cstdio>
#include <string_view>

namespace band3::lights {

namespace {

constexpr std::string_view kUsbPrefix = "usb:";
constexpr std::string_view kPicoPrefix = "pico:";

// "192.168.1.40" in network order (its bytes in address order)
std::optional<uint32_t> ParseAddress(std::string_view text) {
    uint32_t address = 0;
    for (int i = 0; i < 4; i++) {
        if (i) {
            if (text.empty() || text.front() != '.') return std::nullopt;
            text.remove_prefix(1);
        }
        unsigned part = 0;
        const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), part);
        if (error != std::errc{} || end == text.data() || part > 255) return std::nullopt;
        address |= uint32_t(part) << (8 * i);
        text.remove_prefix(end - text.data());
    }
    if (!text.empty()) return std::nullopt;
    return address;
}

std::string Hex(uint8_t value) {
    char text[8];
    std::snprintf(text, sizeof(text), "0x%02X", value);
    return text;
}

}

std::vector<DeviceRow> DeviceRows(const std::vector<KitInfo>& kits,
                                  const std::vector<PicoFleet::Pico>& picos) {
    std::vector<DeviceRow> rows;
    for (const KitInfo& kit : kits) {
        rows.push_back({std::string(kUsbPrefix) + kit.key, "USB", kit.name, "plugged in", true});
    }
    for (const PicoFleet::Pico& pico : picos) {
        const std::string address = FormatAddress(pico.address);
        std::string detail = address + "  ";
        detail += pico.status.usb_status == "Connected" ? "kit: connected" : "no kit";
        detail += pico.online ? "  " + std::to_string(pico.status.wifi_signal) + " dBm"
                              : std::string("  offline");
        rows.push_back({std::string(kPicoPrefix) + address, "Wi-Fi", pico.status.name,
                        std::move(detail), pico.online});
    }
    return rows;
}

bool PicosNeedEvents(const std::vector<PicoFleet::Pico>& picos, bool events_enabled) {
    return !picos.empty() && !events_enabled;
}

Target ParseTarget(const std::optional<std::string>& key) {
    if (!key) return {};
    const std::string_view text = *key;
    if (text.starts_with(kUsbPrefix)) {
        return {Target::Kind::kUsb, std::string(text.substr(kUsbPrefix.size())), 0};
    }
    if (text.starts_with(kPicoPrefix)) {
        if (const auto address = ParseAddress(text.substr(kPicoPrefix.size()))) {
            return {Target::Kind::kPico, "", *address};
        }
    }
    return {};
}

std::string DescribeCommand(Command command) {
    switch (command.right) {
    case kRed: return "red LEDs " + Hex(command.left);
    case kYellow: return "yellow LEDs " + Hex(command.left);
    case kGreen: return "green LEDs " + Hex(command.left);
    case kBlue: return "blue LEDs " + Hex(command.left);
    case kFogOn: return "fog on";
    case kFogOff: return "fog off";
    case kStrobeOff: return "strobe off";
    case kAllOff: return "all off";
    default: break;
    }
    if (command.right >= kStrobeSlow && command.right <= kStrobeFastest) {
        return "strobe " + std::to_string(command.right - 0x02);
    }
    return "command " + Hex(command.right) + " (" + Hex(command.left) + ")";
}

}  // namespace band3::lights
