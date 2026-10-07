#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "stagekit.h"

// The wireless Stage Kits of the RB3E Dashboard's Pico W firmware
// (idio-sync/rb3e-stagekit-networked): a Pico takes RB3E's UDP Stage Kit
// events on 21070 and drives the Stage Kit plugged into it. Sent a discovery
// packet on 21071, it answers there with telemetry, to the last address that
// asked. This is what band3 sends and reads, without a socket.
namespace band3::lights {

inline constexpr uint16_t kRb3ePort = 21070;
inline constexpr uint16_t kTelemetryPort = 21071;
inline constexpr std::string_view kDiscoveryPacket = R"({"type": "discovery"})";

// RB3E's Stage Kit event: the 'RB3E' header (version 0, type 6, two bytes,
// platform unknown), then left and right
std::array<uint8_t, 10> StageKitPacket(Command command);

struct PicoStatus {
    std::string id;          // its MAC address
    std::string name;        // "Pico 1a:2b"
    std::string usb_status;  // "Connected" while a Stage Kit is plugged into it
    int wifi_signal = 0;     // dBm
    bool operator==(const PicoStatus&) const = default;
};

// a Pico's telemetry; nullopt for a discovery packet or anything malformed
std::optional<PicoStatus> ParseTelemetry(std::string_view text);

// "192.168.1.40", from an address in network order
std::string FormatAddress(uint32_t address);

// The Picos heard from, by address: one is offline once it has been quiet for
// 10 seconds and forgotten after 30, as the dashboard lists them.
class PicoFleet {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kOffline{10};
    static constexpr std::chrono::seconds kForgotten{30};

    struct Pico {
        uint32_t address = 0;  // network order
        PicoStatus status;
        bool online = false;
    };

    void Heard(uint32_t address, PicoStatus status, Clock::time_point now);
    // the Picos not yet forgotten, in a steady order; forgets the rest
    std::vector<Pico> List(Clock::time_point now);

private:
    struct Entry {
        PicoStatus status;
        Clock::time_point heard;
    };
    std::map<uint32_t, Entry> picos_;
};

}  // namespace band3::lights
