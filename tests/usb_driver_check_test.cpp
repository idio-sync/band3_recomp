// Checks how band3 sorts known dongles by their Windows driver
// (src/Input/usb_driver_check.h): the ids in a hardware id, which drivers it
// reads through, which dongles it can't read, and what the player is told.

#include <doctest/doctest.h>
#include <optional>
#include <ostream>
#include <string>
#include <vector>
#include "src/Input/usb_driver_check.h"

using namespace band3::input;

namespace band3::input {

std::ostream& operator<<(std::ostream& out, const UsbIds& ids) {
    return out << std::hex << ids.vendor << ':' << ids.product;
}

}

namespace {

constexpr UsbIds kKeys{0x12BA, 0x2330};
constexpr UsbIds kWiiKeys{0x1BAD, 0x3330};

UsbInstrumentEntry Entry(UsbIds ids, std::string service) {
    return {ids, "keyboard", std::move(service), "USB\\VID_12BA&PID_2330\\1"};
}

}

TEST_CASE("a USB hardware id gives its vendor and product") {
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_2330&REV_0100") == kKeys);
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_2330") == kKeys);
    CHECK(ParseUsbHardwareId("usb\\vid_1bad&pid_3330&mi_00") == kWiiKeys);
}

TEST_CASE("ids that aren't a USB device's give nothing") {
    CHECK(ParseUsbHardwareId("") == std::nullopt);
    CHECK(ParseUsbHardwareId("HID\\VID_12BA&PID_2330") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\ROOT_HUB30") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_23") == std::nullopt);
    CHECK(ParseUsbHardwareId("USB\\VID_12BA&PID_ZZZZ") == std::nullopt);
}

TEST_CASE("SDL reads HID's driver and composite parents, the WinUSB reader WinUSB") {
    CHECK(IsHidReadableService("HidUsb"));
    CHECK(IsHidReadableService("hidusb"));
    CHECK(IsHidReadableService("usbccgp"));
    CHECK_FALSE(IsHidReadableService("WinUSB"));
    CHECK_FALSE(IsHidReadableService("libusbK"));
    CHECK_FALSE(IsHidReadableService(""));

    CHECK(IsWinUsbService("WinUSB"));
    CHECK(IsWinUsbService("winusb"));
    CHECK_FALSE(IsWinUsbService("libusbK"));
    CHECK_FALSE(IsWinUsbService("libusb0"));
    CHECK_FALSE(IsWinUsbService("HidUsb"));
}

TEST_CASE("only dongles on drivers band3 can't read are unreadable") {
    const std::vector<UsbInstrumentEntry> entries = {
        Entry(kKeys, "WinUSB"),
        Entry(kWiiKeys, "libusbK"),
        Entry({0x12BA, 0x2430}, "HidUsb"),
        Entry({0x12BA, 0x0200}, ""),
    };
    const auto unreadable = UnreadableInstruments(entries);
    REQUIRE(unreadable.size() == 2);
    CHECK(unreadable[0].ids == kWiiKeys);
    CHECK(unreadable[0].service == "libusbK");
    CHECK(unreadable[1].ids == UsbIds{0x12BA, 0x0200});
    CHECK(unreadable[1].service.empty());
}

TEST_CASE("a composite dongle with one readable interface isn't unreadable") {
    const std::vector<UsbInstrumentEntry> hid = {Entry(kKeys, "usbccgp"), Entry(kKeys, "HidUsb"),
                                                 Entry(kKeys, "SomeVendorUsb")};
    CHECK(UnreadableInstruments(hid).empty());
}

TEST_CASE("two of the same dongle on the same driver are said once") {
    const std::vector<UsbInstrumentEntry> two = {Entry(kWiiKeys, "libusbK"),
                                                 Entry(kWiiKeys, "libusbK")};
    CHECK(UnreadableInstruments(two).size() == 1);
}

TEST_CASE("the message names the device, its driver and the fix") {
    const std::string libusbk =
        WrongDriverMessage({kWiiKeys, "Wii Rock Band 3 keyboard", "libusbK"});
    CHECK(libusbk.find("Wii Rock Band 3 keyboard (1BAD:3330)") != std::string::npos);
    CHECK(libusbk.find("Zadig's libusbK driver") != std::string::npos);
    CHECK(libusbk.find("choose WinUSB") != std::string::npos);

    CHECK(WrongDriverMessage({kKeys, "x", "libusb0"}).find("Zadig's libusb-win32") !=
          std::string::npos);

    // only Zadig's drivers are said to be Zadig's
    const std::string other = WrongDriverMessage({kKeys, "x", "SomeVendorUsb"});
    CHECK(other.find("the SomeVendorUsb driver,") != std::string::npos);
    CHECK(other.find("Delete the driver software") != std::string::npos);

    const std::string none = WrongDriverMessage({kKeys, "x", ""});
    CHECK(none.find("no driver") != std::string::npos);
}
