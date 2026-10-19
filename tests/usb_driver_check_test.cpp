// Checks how band3 spots a known dongle on another Windows driver
// (src/Input/usb_driver_check.h): the ids in its hardware id, which drivers
// SDL's HID API can read through, and what the player is told.

#include <doctest/doctest.h>
#include <optional>
#include <ostream>
#include <string>
#include "src/Input/usb_driver_check.h"

using namespace band3::input;

namespace band3::input {

std::ostream& operator<<(std::ostream& out, const UsbIds& ids) {
    return out << std::hex << ids.vendor << ':' << ids.product;
}

}

TEST_CASE("a USB hardware id gives its vendor and product") {
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_2330&REV_0100") == UsbIds{0x12BA, 0x2330});
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_2330") == UsbIds{0x12BA, 0x2330});
    CHECK(ParseUsbHardwareId("usb\\vid_1bad&pid_3330&mi_00") == UsbIds{0x1BAD, 0x3330});
}

TEST_CASE("ids that aren't a USB device's give nothing") {
    CHECK(ParseUsbHardwareId("") == std::nullopt);
    CHECK(ParseUsbHardwareId("HID\\VID_12BA&PID_2330") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\ROOT_HUB30") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_23") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_ZZZZ") == std::nullopt);
}

TEST_CASE("HID's driver and composite parents are readable, Zadig's aren't") {
    CHECK(IsHidReadableService("HidUsb"));
    CHECK(IsHidReadableService("hidusb"));
    CHECK(IsHidReadableService("usbccgp"));
    CHECK_FALSE(IsHidReadableService("WinUSB"));
    CHECK_FALSE(IsHidReadableService("libusbK"));
    CHECK_FALSE(IsHidReadableService("libusb0"));
    CHECK_FALSE(IsHidReadableService(""));
}

TEST_CASE("the message names the device, its driver and the way back") {
    const std::string winusb =
        WrongDriverMessage({{0x12BA, 0x2330}, "PS3 Rock Band 3 keyboard", "WinUSB"});
    CHECK(winusb.find("PS3 Rock Band 3 keyboard (12BA:2330)") != std::string::npos);
    CHECK(winusb.find("WinUSB driver") != std::string::npos);
    CHECK(winusb.find("Delete the driver software") != std::string::npos);

    CHECK(winusb.find("Zadig's") != std::string::npos);

    CHECK(WrongDriverMessage({{0x12BA, 0x2330}, "x", "libusb0"}).find("libusb-win32") !=
          std::string::npos);

    // only Zadig's drivers are said to be Zadig's
    const std::string other = WrongDriverMessage({{0x12BA, 0x2330}, "x", "SomeVendorUsb"});
    CHECK(other.find("SomeVendorUsb driver,") != std::string::npos);
    CHECK(other.find("Zadig") == std::string::npos);

    const std::string none = WrongDriverMessage({{0x1BAD, 0x3330}, "Wii Rock Band 3 keyboard", ""});
    CHECK(none.find("no driver") != std::string::npos);
    CHECK(none.find("Zadig") == std::string::npos);
}
