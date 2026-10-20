#include "hotspot_plan.h"

namespace band3::lights::hotspot {

namespace {

// the connection's settings, after add's or modify's own arguments. 2.4 GHz,
// as a Pico W has no 5 GHz radio; WPA2 alone with protected management frames
// off, which a Pico W's driver doesn't do; addresses as Addressing says, and
// no IPv6. Not started by NetworkManager on its own: band3 starts and stops it
void AddSettings(std::vector<std::string>& args, const Config& config, Addressing addressing) {
    const std::initializer_list<std::string> settings = {
        "connection.autoconnect", "no",
        "802-11-wireless.ssid", config.name,
        "802-11-wireless.mode", "ap",
        "802-11-wireless.band", "bg",
        "ipv4.method", addressing == Addressing::kDhcp ? "shared" : "link-local",
        "ipv6.method", "ignore",
        "wifi-sec.key-mgmt", "wpa-psk",
        "wifi-sec.proto", "rsn",
        "wifi-sec.pairwise", "ccmp",
        "wifi-sec.group", "ccmp",
        "wifi-sec.pmf", "disable",
        "wifi-sec.psk", config.password,
    };
    args.insert(args.end(), settings);
}

}

bool ValidName(std::string_view name) { return !name.empty() && name.size() <= 32; }

bool ValidPassword(std::string_view password) {
    if (password.size() < 8 || password.size() > 63) return false;
    for (const char c : password) {
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

std::string ConfigProblem(const Config& config) {
    if (!ValidName(config.name)) return "Give the hotspot a network name of up to 32 characters.";
    if (config.password.empty()) return "Set a password of 8 to 63 characters to start the hotspot.";
    if (!ValidPassword(config.password)) {
        return "The password needs 8 to 63 characters, letters, digits and symbols only.";
    }
    return {};
}

std::vector<std::string> SplitTerse(std::string_view line) {
    std::vector<std::string> fields(1);
    for (size_t i = 0; i < line.size(); i++) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            fields.back() += line[++i];
        } else if (line[i] == ':') {
            fields.emplace_back();
        } else {
            fields.back() += line[i];
        }
    }
    return fields;
}

std::optional<WifiDevice> FindWifiDevice(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        const auto fields = SplitTerse(line);
        if (fields.size() < 3 || fields[1] != "wifi") continue;
        return WifiDevice{fields[0], fields[2] != "unavailable"};
    }
    return std::nullopt;
}

bool ListsConnection(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        if (SplitTerse(line)[0] == kConnection) return true;
    }
    return false;
}

std::vector<std::string> DevicesArgs() { return {"-t", "-f", "DEVICE,TYPE,STATE", "device"}; }

std::vector<std::string> ActiveArgs() {
    return {"-t", "-f", "NAME", "connection", "show", "--active"};
}

std::vector<std::string> AddArgs(const Config& config, std::string_view device,
                                 Addressing addressing) {
    std::vector<std::string> args = {"connection", "add", "type", "wifi", "con-name",
                                     std::string(kConnection), "ifname", std::string(device)};
    AddSettings(args, config, addressing);
    return args;
}

std::vector<std::string> ModifyArgs(const Config& config, Addressing addressing) {
    std::vector<std::string> args = {"connection", "modify", std::string(kConnection)};
    AddSettings(args, config, addressing);
    return args;
}

std::vector<std::string> UpArgs(std::string_view device) {
    return {"--wait", "30", "connection", "up", std::string(kConnection), "ifname",
            std::string(device)};
}

std::vector<std::string> DownArgs() {
    return {"connection", "down", std::string(kConnection)};
}

std::vector<std::string> AddressArgs(std::string_view device) {
    return {"-g", "IP4.ADDRESS", "device", "show", std::string(device)};
}

std::string FirstAddress(const std::vector<std::string>& lines) {
    for (const std::string& line : lines) {
        // several addresses come on one line, separated by " | "
        std::string_view text = line;
        text = text.substr(0, text.find(' '));
        text = text.substr(0, text.find('/'));
        if (!text.empty()) return std::string(text);
    }
    return {};
}

std::string ErrorText(const std::vector<std::string>& lines) {
    constexpr std::string_view kError = "Error: ";
    for (const std::string& line : lines) {
        if (line.starts_with(kError)) return line.substr(kError.size());
    }
    return lines.empty() ? std::string() : lines.back();
}

}
