#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "kit_sender.h"
#include "pico_fleet.h"

// What the Lights tab shows of the Stage Kits found, and how it names the
// one a test command is for. Pure: lights.cpp hands it the lists.
namespace band3::lights {

struct DeviceRow {
    std::string key;     // what a test command names it by: "usb:<kit key>", "pico:<address>"
    std::string kind;    // "USB", "Wi-Fi"
    std::string name;
    std::string detail;  // "plugged in", "192.168.1.40  kit: connected  -52 dBm"
    bool online = true;
    bool operator==(const DeviceRow&) const = default;
};

// the USB kits, then the Picos
std::vector<DeviceRow> DeviceRows(const std::vector<KitInfo>& kits,
                                  const std::vector<PicoFleet::Pico>& picos);

// Picos follow the game through the RB3Enhanced events: whether to say
// they're off while a Pico is listed
bool PicosNeedEvents(const std::vector<PicoFleet::Pico>& picos, bool events_enabled);

// the device a test command is for, from its row's key; nullopt is all of them
struct Target {
    enum class Kind { kAll, kUsb, kPico } kind = Kind::kAll;
    std::string usb_key;
    uint32_t address = 0;  // network order
    bool operator==(const Target&) const = default;
};
Target ParseTarget(const std::optional<std::string>& key);

// a command in words, for "Sent:": "red LEDs 0x55", "fog on", "strobe 2", "all off"
std::string DescribeCommand(Command command);

}  // namespace band3::lights
