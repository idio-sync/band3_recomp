#pragma once
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Which driver Windows has bound each plugged-in dongle band3 knows
// (KnownHidInstruments) to. SDL's HID API reads the ones under HID's driver,
// band3's WinUSB reader (winusb_instruments.h) the ones under Zadig's WinUSB,
// which RPCS3's USB passthrough needs (the milohax RB3 guides set the pro
// instruments up that way). Any other driver is said in the log and the
// Instrument Lab instead of the dongle just not showing up.

namespace band3::input {

struct UsbIds {
    uint16_t vendor = 0;
    uint16_t product = 0;
    bool operator==(const UsbIds&) const = default;
};

// the vendor and product in a USB hardware id such as
// "USB\VID_12BA&PID_2330&REV_0100" (or a composite device's "...&MI_00")
std::optional<UsbIds> ParseUsbHardwareId(std::string_view id);

// whether a device under this Windows driver service is fine for SDL's HID
// API: HID's own (HidUsb), or a composite device's parent (usbccgp), whose
// interfaces are listed separately with their own drivers
bool IsHidReadableService(std::string_view service);

// whether it's WinUSB, which band3's WinUSB reader opens
bool IsWinUsbService(std::string_view service);

// a known instrument as Windows lists it
struct UsbInstrumentEntry {
    UsbIds ids;
    std::string name;
    // its Windows driver service, empty if it has none
    std::string service;
    // its device instance id, e.g. "USB\VID_12BA&PID_2330\6&2b3c..."
    std::string instance;
};

// Windows: the known HID instruments plugged in right now, one entry per
// device and interface. Empty elsewhere.
std::vector<UsbInstrumentEntry> ListKnownUsbInstruments();

// a known instrument plugged in under a driver band3 can't read
struct WrongDriverInstrument {
    UsbIds ids;
    std::string name;
    std::string service;
};

// the instruments in `entries` neither SDL's HID API nor the WinUSB reader
// can read: a composite dongle with one readable interface is fine
std::vector<WrongDriverInstrument> UnreadableInstruments(std::span<const UsbInstrumentEntry> entries);

// what to tell the player about it, and how to fix it
std::string WrongDriverMessage(const WrongDriverInstrument& instrument);

}
