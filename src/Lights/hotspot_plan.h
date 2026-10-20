#pragma once
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The Steam Deck's own Wi-Fi network for the wireless Stage Kits (hotspot.h),
// apart from running it: what the settings may be, the nmcli commands that
// make, start and stop it, and reading what nmcli prints. Pure.

namespace band3::lights::hotspot {

// NetworkManager's name for the connection band3 keeps
inline constexpr std::string_view kConnection = "band3 hotspot";

struct Config {
    std::string name;
    std::string password;
    bool operator==(const Config&) const = default;
};

// a network name (SSID): 1 to 32 bytes
bool ValidName(std::string_view name);
// a WPA2 passphrase: 8 to 63 printable ASCII characters
bool ValidPassword(std::string_view password);
// why the hotspot can't start with `config`, in words; empty when it can
std::string ConfigProblem(const Config& config);

// nmcli's terse (-t) line split at its colons, its "\:" and "\\" unescaped
std::vector<std::string> SplitTerse(std::string_view line);

struct WifiDevice {
    std::string name;  // "wlan0"
    // false while the Wi-Fi is switched off (airplane mode)
    bool available = true;
};
// the first Wi-Fi device in `nmcli -t -f DEVICE,TYPE,STATE device`'s lines
std::optional<WifiDevice> FindWifiDevice(const std::vector<std::string>& lines);

// whether `nmcli -t -f NAME connection show --active`'s lines list kConnection
bool ListsConnection(const std::vector<std::string>& lines);

// how the devices that join get their addresses
enum class Addressing {
    // NetworkManager's DHCP (10.42.0.x), which it runs with dnsmasq
    kDhcp,
    // without dnsmasq, which SteamOS doesn't have: link-local (169.254.x.x),
    // which the Deck picks for itself and a device that joins has to as well
    // (lwIP's AutoIP, on a Pico W), once DHCP doesn't answer
    kLinkLocal,
};
// where NetworkManager looks for dnsmasq, which kDhcp needs
inline constexpr std::string_view kDnsmasqPaths[] = {
    "/usr/bin/dnsmasq", "/usr/sbin/dnsmasq", "/sbin/dnsmasq", "/usr/local/sbin/dnsmasq",
    "/usr/local/bin/dnsmasq"};

// nmcli's arguments
std::vector<std::string> DevicesArgs();
std::vector<std::string> ActiveArgs();
// makes the connection, on `device`
std::vector<std::string> AddArgs(const Config& config, std::string_view device,
                                 Addressing addressing);
// sets an existing one to `config`
std::vector<std::string> ModifyArgs(const Config& config, Addressing addressing);
std::vector<std::string> UpArgs(std::string_view device);
std::vector<std::string> DownArgs();
std::vector<std::string> AddressArgs(std::string_view device);

// nmcli's exit code when the connection (or device) isn't there
inline constexpr int kNotFound = 10;

// "10.42.0.1/24" from `nmcli -g IP4.ADDRESS device show`'s lines: "10.42.0.1"
std::string FirstAddress(const std::vector<std::string>& lines);
// what nmcli said went wrong: its "Error: ..." line, without "Error: ", or
// its last line
std::string ErrorText(const std::vector<std::string>& lines);

}
