#include "hid_capture.h"
#include <charconv>
#include <cstdio>

namespace band3::input {

namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

template <typename T>
bool ParseNumber(std::string_view s, T& out, int base) {
    s = Trim(s);
    if (s.empty()) return false;
    auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), out, base);
    return ec == std::errc() && end == s.data() + s.size();
}

}

std::string FormatHidCapture(const HidCapture& capture) {
    std::string out = "# band3 HID capture\n";
    char line[64];
    out += "device: " + capture.device + "\n";
    std::snprintf(line, sizeof(line), "usb: %04X:%04X\nrelease: %04X\n---\n", capture.vendor,
                  capture.product, capture.release);
    out += line;
    for (const auto& report : capture.reports) {
        out += std::to_string(report.ms);
        for (uint8_t b : report.bytes) {
            std::snprintf(line, sizeof(line), " %02x", b);
            out += line;
        }
        out += '\n';
    }
    return out;
}

std::optional<HidCapture> ParseHidCapture(std::string_view text) {
    HidCapture capture;
    bool in_reports = false;
    bool has_usb = false;

    while (!text.empty()) {
        const size_t newline = text.find('\n');
        std::string_view line = Trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);
        if (line.empty() || line.front() == '#') continue;

        if (!in_reports) {
            if (line == "---") {
                in_reports = true;
                continue;
            }
            const size_t colon = line.find(':');
            if (colon == std::string_view::npos) return std::nullopt;
            const std::string_view key = Trim(line.substr(0, colon));
            const std::string_view value = Trim(line.substr(colon + 1));
            if (key == "device") {
                capture.device = std::string(value);
            } else if (key == "usb") {
                const size_t sep = value.find(':');
                if (sep == std::string_view::npos ||
                    !ParseNumber(value.substr(0, sep), capture.vendor, 16) ||
                    !ParseNumber(value.substr(sep + 1), capture.product, 16)) {
                    return std::nullopt;
                }
                has_usb = true;
            } else if (key == "release") {
                if (!ParseNumber(value, capture.release, 16)) return std::nullopt;
            }
            // other keys are for people reading it
            continue;
        }

        HidCapture::Report report;
        size_t space = line.find(' ');
        if (!ParseNumber(line.substr(0, space), report.ms, 10)) return std::nullopt;
        while (space != std::string_view::npos) {
            line = Trim(line.substr(space + 1));
            if (line.empty()) break;
            space = line.find(' ');
            uint8_t byte = 0;
            if (!ParseNumber(line.substr(0, space), byte, 16)) return std::nullopt;
            report.bytes.push_back(byte);
        }
        capture.reports.push_back(std::move(report));
    }

    if (!in_reports || !has_usb) return std::nullopt;
    return capture;
}

}
