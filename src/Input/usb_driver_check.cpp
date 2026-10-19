#include "usb_driver_check.h"
#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstdio>

namespace band3::input {

namespace {

bool EqualsNoCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::tolower(static_cast<unsigned char>(x)) ==
                      std::tolower(static_cast<unsigned char>(y));
           });
}

// the four hex digits after `key` (e.g. "VID_") in `id`, ignoring case
std::optional<uint16_t> HexAfter(std::string_view id, std::string_view key) {
    for (size_t at = 0; at + key.size() + 4 <= id.size(); at++) {
        if (!EqualsNoCase(id.substr(at, key.size()), key)) continue;
        const char* first = id.data() + at + key.size();
        uint16_t value = 0;
        const auto [end, error] = std::from_chars(first, first + 4, value, 16);
        if (error != std::errc{} || end != first + 4) return std::nullopt;
        return value;
    }
    return std::nullopt;
}

// what one of Zadig's driver choices is called in its list, or nothing for
// any other driver
std::optional<std::string_view> ZadigDriverLabel(std::string_view service) {
    if (EqualsNoCase(service, "WinUSB")) return "WinUSB";
    if (EqualsNoCase(service, "libusbK")) return "libusbK";
    if (EqualsNoCase(service, "libusb0")) return "libusb-win32";
    return std::nullopt;
}

}

std::optional<UsbIds> ParseUsbHardwareId(std::string_view id) {
    if (id.size() < 4 || !EqualsNoCase(id.substr(0, 4), "USB\\")) return std::nullopt;
    const auto vendor = HexAfter(id, "VID_");
    const auto product = HexAfter(id, "PID_");
    if (!vendor || !product) return std::nullopt;
    return UsbIds{*vendor, *product};
}

bool IsHidReadableService(std::string_view service) {
    return EqualsNoCase(service, "HidUsb") || EqualsNoCase(service, "usbccgp");
}

bool IsWinUsbService(std::string_view service) { return EqualsNoCase(service, "WinUSB"); }

std::vector<WrongDriverInstrument> UnreadableInstruments(std::span<const UsbInstrumentEntry> entries) {
    const auto readable = [](const UsbInstrumentEntry& entry) {
        return IsHidReadableService(entry.service) || IsWinUsbService(entry.service);
    };
    std::vector<WrongDriverInstrument> out;
    for (const auto& entry : entries) {
        if (readable(entry)) continue;
        const bool sibling_readable = std::ranges::any_of(entries, [&](const UsbInstrumentEntry& other) {
            return other.ids == entry.ids && readable(other);
        });
        const bool listed = std::ranges::any_of(out, [&](const WrongDriverInstrument& wrong) {
            return wrong.ids == entry.ids && wrong.service == entry.service;
        });
        if (!sibling_readable && !listed) out.push_back({entry.ids, entry.name, entry.service});
    }
    return out;
}

std::string WrongDriverMessage(const WrongDriverInstrument& instrument) {
    char ids[16];
    std::snprintf(ids, sizeof(ids), "%04X:%04X", instrument.ids.vendor, instrument.ids.product);
    std::string message = instrument.name + " (" + ids + ") is plugged in, but ";
    if (instrument.service.empty()) {
        return message + "Windows has no driver for it, so band3 can't read it. band3 reads it "
                         "through Windows' own HID driver or Zadig's WinUSB: in Device Manager, "
                         "uninstall the device, then unplug it and plug it back in.";
    }
    if (const auto zadig = ZadigDriverLabel(instrument.service)) {
        return message + "it uses Zadig's " + std::string(*zadig) +
               " driver, which band3 can't read. In Zadig, pick the device, choose WinUSB and "
               "click Replace Driver: band3 reads WinUSB, and RPCS3's USB passthrough works "
               "with it too.";
    }
    return message + "it uses the " + instrument.service +
           " driver, so band3 can't read it. band3 reads it through Windows' own HID driver or "
           "Zadig's WinUSB: in Device Manager, uninstall the device with \"Delete the driver "
           "software for this device\" ticked, then unplug it and plug it back in.";
}

}
