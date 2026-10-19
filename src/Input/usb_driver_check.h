#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Finds the dongles band3 knows (KnownHidInstruments) that Windows has bound
// to some driver other than its HID one, which SDL's HID API can't see. Most
// often that's Zadig's WinUSB, which RPCS3's USB passthrough needs (the
// milohax RB3 guides set the pro instruments up that way). The driver says so
// in the log and the Instrument Lab instead of the dongle just not showing up.

namespace band3::input {

struct UsbIds {
    uint16_t vendor = 0;
    uint16_t product = 0;
    bool operator==(const UsbIds&) const = default;
};

// the vendor and product in a USB hardware id such as
// "USB\VID_12BA&PID_2330&REV_0100" (or a composite device's "...&MI_00")
std::optional<UsbIds> ParseUsbHardwareId(std::string_view id);

// whether a device under this Windows driver service is fine for band3: HID's
// own (HidUsb), or a composite device's parent (usbccgp), whose interfaces
// are listed separately with their own drivers
bool IsHidReadableService(std::string_view service);

// a known instrument plugged in under some other driver
struct WrongDriverInstrument {
    UsbIds ids;
    std::string name;
    // its Windows driver service, empty if it has none
    std::string service;
};

// what to tell the player about it, and how to put HID's driver back
std::string WrongDriverMessage(const WrongDriverInstrument& instrument);

// Windows: the known HID instruments plugged in right now under another
// driver. Empty elsewhere.
std::vector<WrongDriverInstrument> FindInstrumentsOnOtherDrivers();

}
