// Checks what the Lights tab shows of the Stage Kits found
// (src/Lights/lights_view.cpp).

#include <doctest/doctest.h>
#include "src/Lights/lights_view.h"

using namespace band3::lights;

namespace {

uint32_t Address(uint8_t last) { return 192u | 168u << 8 | 1u << 16 | uint32_t(last) << 24; }

PicoFleet::Pico Pico(uint8_t last, std::string usb, bool online, int signal = -52) {
    return {Address(last), {"28:cd", "Pico 1a:2b", std::move(usb), signal}, online};
}

}

TEST_CASE("the USB kits are listed, then the Picos") {
    const auto rows = DeviceRows(
        {{"hid:\\\\?\\hid#vid_1209", "Santroller Stage Kit (HID)", KitKind::kHid},
         {"xinput:2", "Stage Kit (XInput, slot 3)", KitKind::kXInput}},
        {Pico(40, "Connected", true), Pico(41, "Disconnected", false)});
    REQUIRE(rows.size() == 4);
    CHECK(rows[0] == DeviceRow{"usb:hid:\\\\?\\hid#vid_1209", "USB",
                               "Santroller Stage Kit (HID)", "plugged in", true});
    CHECK(rows[1].key == "usb:xinput:2");
    CHECK(rows[2] == DeviceRow{"pico:192.168.1.40", "Wi-Fi", "Pico 1a:2b",
                               "192.168.1.40  kit: connected  -52 dBm", true});
    CHECK(rows[3] == DeviceRow{"pico:192.168.1.41", "Wi-Fi", "Pico 1a:2b",
                               "192.168.1.41  no kit  offline", false});
}

TEST_CASE("the events hint shows only for Picos with the events off") {
    CHECK_FALSE(PicosNeedEvents({}, false));
    CHECK(PicosNeedEvents({Pico(40, "Connected", true)}, false));
    CHECK_FALSE(PicosNeedEvents({Pico(40, "Connected", true)}, true));
}

TEST_CASE("a row's key names the device a test command is for") {
    CHECK(ParseTarget(std::nullopt) == Target{});
    CHECK(ParseTarget(std::string("usb:xinput:2")) ==
          Target{Target::Kind::kUsb, "xinput:2", 0});
    CHECK(ParseTarget(std::string("pico:192.168.1.40")) ==
          Target{Target::Kind::kPico, "", Address(40)});
    // anything else is every device, as is a Pico's address that isn't one
    CHECK(ParseTarget(std::string("pico:not an address")) == Target{});
    CHECK(ParseTarget(std::string("what")) == Target{});
}

TEST_CASE("commands in words") {
    CHECK(DescribeCommand({0x55, kRed}) == "red LEDs 0x55");
    CHECK(DescribeCommand({0xFF, kBlue}) == "blue LEDs 0xFF");
    CHECK(DescribeCommand({0x00, kYellow}) == "yellow LEDs 0x00");
    CHECK(DescribeCommand({0x0F, kGreen}) == "green LEDs 0x0F");
    CHECK(DescribeCommand({0, kFogOn}) == "fog on");
    CHECK(DescribeCommand({0, kFogOff}) == "fog off");
    CHECK(DescribeCommand({0, 0x04}) == "strobe 2");
    CHECK(DescribeCommand({0, kStrobeOff}) == "strobe off");
    CHECK(DescribeCommand(kAllOffCommand) == "all off");
    CHECK(DescribeCommand({0x12, 0x10}) == "command 0x10 (0x12)");
}
