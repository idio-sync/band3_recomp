#pragma once
#include <memory>
#include <rex/input/input_driver.h>

// PS3 and Wii Rock Band instruments, read straight from their USB dongles with
// SDL's HID API and reported as Xbox 360 instruments (see ps3_instruments.h).
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

}
