#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// A recording of one HID instrument's raw reports, saved from the Instrument
// Lab so problems with a device can be looked at, and replayed in tests,
// without the hardware. Stored as text:
//
//   # band3 HID capture
//   device: PS3 Rock Band drums
//   usb: 12BA:0210
//   release: 0200
//   ---
//   0 00 00 08 7f 7f 7f 7f ...
//   16 04 04 08 7f 7f 7f 7f ...
//
// Each report line is the milliseconds since the capture started, then the
// report's bytes exactly as read. Lines starting with # are comments.

namespace band3::input {

struct HidCapture {
    struct Report {
        uint32_t ms = 0;
        std::vector<uint8_t> bytes;
    };

    std::string device;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t release = 0;
    std::vector<Report> reports;
};

std::string FormatHidCapture(const HidCapture& capture);

// nullopt if the text isn't a capture
std::optional<HidCapture> ParseHidCapture(std::string_view text);

}
