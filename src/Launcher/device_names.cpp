#include "device_names.h"
#include <algorithm>
#include <cctype>
#include <string>
#include "src/Audio/usb_mic.h"

namespace band3::launcher {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

}

std::optional<size_t> FindSavedDevice(std::span<const std::string> names, std::string_view saved) {
    saved = Trim(saved);
    if (saved.empty()) return std::nullopt;
    for (size_t i = 0; i < names.size(); i++) {
        if (audio::usb_mic::NameMatches(names[i], saved)) return i;
    }
    return std::nullopt;
}

std::optional<size_t> FindMidiPort(std::span<const std::string> ports, std::string_view saved) {
    // untrimmed, as the driver takes it
    for (size_t i = 0; i < ports.size(); i++) {
        if (saved.empty() ? !audio::usb_mic::NameMatches(ports[i], "through")
                          : audio::usb_mic::NameMatches(ports[i], saved)) {
            return i;
        }
    }
    return std::nullopt;
}

std::string StripMidiPortIndex(std::string_view port, unsigned index) {
    const std::string suffix = " " + std::to_string(index);
    if (port.size() > suffix.size() && port.ends_with(suffix)) {
        port.remove_suffix(suffix.size());
    }
    return std::string(port);
}

std::string MicSlotValue(std::string_view device) {
    if (device.find(',') == std::string_view::npos) return std::string(Trim(device));
    std::string_view best;
    while (true) {
        const size_t comma = device.find(',');
        const std::string_view part = Trim(device.substr(0, comma));
        if (part.size() > best.size()) best = part;
        if (comma == std::string_view::npos) break;
        device.remove_prefix(comma + 1);
    }
    return std::string(best);
}

std::vector<size_t> SdlDisplayOrder(const std::vector<bool>& primary) {
    std::vector<size_t> order;
    order.reserve(primary.size());
    for (bool want_primary : {true, false}) {
        for (size_t i = 0; i < primary.size(); i++) {
            if (primary[i] == want_primary) order.push_back(i);
        }
    }
    return order;
}

std::vector<DisplayMode> SortDisplayModes(std::vector<DisplayMode> modes) {
    std::erase_if(modes, [](const DisplayMode& m) { return m.width <= 0 || m.height <= 0; });
    std::ranges::sort(modes, [](const DisplayMode& a, const DisplayMode& b) {
        if (a.width != b.width) return a.width > b.width;
        if (a.height != b.height) return a.height > b.height;
        return a.refresh_hz > b.refresh_hz;
    });
    const auto [first, last] = std::ranges::unique(modes);
    modes.erase(first, last);
    return modes;
}

}
