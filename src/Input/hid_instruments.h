#pragma once
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include <rex/input/input_driver.h>
#include "instruments.h"
#include "hid_instrument_types.h"
#include "usb_driver_check.h"

// PlayStation and Wii Rock Band instruments (hid_instrument_types.h lists them),
// read straight from their USB dongles with SDL's HID API (or on Windows,
// through WinUSB for the dongles Zadig put on it: winusb_instruments.h), and
// on Windows Xbox One ones, read through GameInput (gameinput_instruments.h),
// all reported as Xbox 360 instruments.
// Turned on by the hid_instruments setting. On Linux the dongles' hidraw nodes
// need to be readable, which usually takes a udev rule.

namespace band3::input {

std::unique_ptr<rex::input::InputDriver> CreateHidInstrumentDriver();

// one of the HID driver's instruments
bool IsHidInstrument(const rex::input::DeviceInfo& device);

// whether another driver's device (an SDL gamepad) is an instrument the HID
// driver is reading, going by the USB ids in its SDL GUID, so the player
// assignment can leave it out instead of it also showing up as a controller
bool IsSdlCopyOfHidInstrument(const rex::input::DeviceInfo& device);

// For the Instrument Lab.

// whether the HID driver is running
bool HidInstrumentsActive();

struct HidInstrumentStatus {
    uint64_t id = 0;
    std::string name;
    uint16_t vendor = 0;
    uint16_t product = 0;
    uint16_t release = 0;
    HidInstrumentType instrument = HidInstrumentType::kPs3Guitar;
    uint64_t report_count = 0;
    std::vector<uint8_t> last_report;
    Gamepad360 state;
    bool capturing = false;
};

// the instruments open right now
std::vector<HidInstrumentStatus> HidInstrumentStatuses();

// the known dongles plugged in under a driver band3 can't read
// (usb_driver_check.h), as of the driver's last look, every few seconds
std::vector<WrongDriverInstrument> HidInstrumentsOnOtherDrivers();

// Records the instrument's raw reports for `length`, then saves them to
// logs/hid-capture-<usb id>-<time>.txt (see hid_capture.h). False if the
// instrument is gone or already capturing.
bool StartHidCapture(uint64_t id, std::chrono::seconds length);

// what happened to the last capture: where it was saved, or why it wasn't
std::string LastHidCaptureMessage();

}
