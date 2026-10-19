#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include "report_source.h"
#include "usb_driver_check.h"

// Dongles Zadig has put on WinUSB (for RPCS3's USB passthrough), which SDL's
// HID API can't see, read through WinUSB instead for the instrument driver
// (hid_instruments.h): the reports from their interrupt-in pipe, and the PS3
// pro instruments' enable report as HID SET_REPORT control transfers. Only
// one program can have a WinUSB device open, so RPCS3 can't use it while
// band3 runs. Windows only.

namespace band3::input {

struct WinUsbInstrument {
    // the device descriptor's, which may differ from the entry's on a
    // composite dongle
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t release = 0;
    std::unique_ptr<ReportSource> source;
};

// Opens a known instrument WinUSB is bound to (ListKnownUsbInstruments' entry
// with IsWinUsbService), asking a PS3 pro instrument for its keys or frets.
// On failure says why in `error` and returns nullptr's source.
WinUsbInstrument OpenWinUsbInstrument(const UsbInstrumentEntry& entry, std::string& error);

}
