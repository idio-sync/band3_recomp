#include "midi_port_select.h"
#include <algorithm>
#include <cctype>

namespace band3::input {

namespace {

std::string Lower(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s;
}

bool Contains(const std::string& port, const std::string& part) {
    return Lower(port).find(Lower(part)) != std::string::npos;
}

}

std::optional<size_t> PickPort(const std::vector<std::string>& ports, const std::string& wanted,
                               const std::vector<std::string>& taken) {
    for (size_t i = 0; i < ports.size(); i++) {
        if (!wanted.empty()) {
            if (Contains(ports[i], wanted)) return i;
            continue;
        }
        if (Contains(ports[i], "through")) continue;
        if (std::ranges::find(taken, ports[i]) != taken.end()) continue;
        return i;
    }
    return std::nullopt;
}

}
