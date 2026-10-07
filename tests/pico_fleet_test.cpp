// Checks what band3 sends the Pico W Stage Kits and reads back from them
// (src/Lights/pico_fleet.cpp): the Stage Kit event, telemetry, and how long a
// quiet Pico stays listed.

#include <doctest/doctest.h>
#include "src/Lights/pico_fleet.h"

using namespace band3::lights;

namespace {

using Clock = PicoFleet::Clock;

Clock::time_point At(int64_t ms) { return Clock::time_point{} + std::chrono::milliseconds(ms); }

// 192.168.1.<last> in network order (its bytes in address order)
uint32_t Address(uint8_t last) {
    const uint8_t bytes[4] = {192, 168, 1, last};
    uint32_t address = 0;
    for (int i = 0; i < 4; i++) address |= uint32_t(bytes[i]) << (8 * i);
    return address;
}

PicoStatus Status(std::string name, std::string usb = "Connected", int signal = -52) {
    return {"28:cd:c1:00:1a:2b", std::move(name), std::move(usb), signal};
}

}

TEST_CASE("the Stage Kit event a Pico takes") {
    CHECK(StageKitPacket({0x55, kRed}) ==
          std::array<uint8_t, 10>{'R', 'B', '3', 'E', 0, 6, 2, 0xFF, 0x55, 0x80});
}

TEST_CASE("a Pico's telemetry, as its firmware sends it") {
    const auto status = ParseTelemetry(
        R"({"id":"28:cd:c1:00:1a:2b","name":"Pico 1a:2b","usb_status":"Connected",)"
        R"("wifi_signal":-52,"uptime":3600})");
    REQUIRE(status);
    CHECK(*status == PicoStatus{"28:cd:c1:00:1a:2b", "Pico 1a:2b", "Connected", -52});
}

TEST_CASE("telemetry that isn't a Pico's is ignored") {
    // band3's own discovery, heard back on a broadcast
    CHECK_FALSE(ParseTelemetry(kDiscoveryPacket));
    CHECK_FALSE(ParseTelemetry(R"({"type":"discovery"})"));
    CHECK_FALSE(ParseTelemetry("not json"));
    CHECK_FALSE(ParseTelemetry("[1,2]"));
    CHECK_FALSE(ParseTelemetry(R"({"usb_status":"Connected","wifi_signal":-52})"));
    CHECK_FALSE(ParseTelemetry(R"({"name":"Pico","wifi_signal":-52})"));
    CHECK_FALSE(ParseTelemetry(R"({"name":"Pico","usb_status":"Connected"})"));
    CHECK_FALSE(ParseTelemetry(R"({"name":"Pico","usb_status":"Connected","wifi_signal":"-52"})"));
    CHECK_FALSE(ParseTelemetry(R"({"name":"Pico","usb_status":"Connected","wifi_signal":-52.5})"));
}

TEST_CASE("telemetry without an id is still a Pico's") {
    const auto status =
        ParseTelemetry(R"({"name":"Pico","usb_status":"Disconnected","wifi_signal":-70})");
    REQUIRE(status);
    CHECK(status->id.empty());
    CHECK(status->usb_status == "Disconnected");
}

TEST_CASE("addresses are written as dotted quads") {
    CHECK(FormatAddress(Address(40)) == "192.168.1.40");
}

TEST_CASE("a Pico is online until it's been quiet for 10 seconds, and forgotten after 30") {
    PicoFleet fleet;
    fleet.Heard(Address(40), Status("Pico 1a:2b"), At(0));

    auto list = fleet.List(At(9'999));
    REQUIRE(list.size() == 1);
    CHECK(list[0].address == Address(40));
    CHECK(list[0].status.name == "Pico 1a:2b");
    CHECK(list[0].online);

    list = fleet.List(At(10'000));
    REQUIRE(list.size() == 1);
    CHECK_FALSE(list[0].online);

    CHECK(fleet.List(At(30'000)).empty());
    // and stays forgotten
    CHECK(fleet.List(At(1'000)).empty());
}

TEST_CASE("hearing from a Pico again keeps it and takes its new telemetry") {
    PicoFleet fleet;
    fleet.Heard(Address(40), Status("Pico 1a:2b", "Connected"), At(0));
    fleet.Heard(Address(41), Status("Pico 3c:4d"), At(0));
    fleet.Heard(Address(40), Status("Pico 1a:2b", "Disconnected"), At(25'000));

    const auto list = fleet.List(At(31'000));
    REQUIRE(list.size() == 1);
    CHECK(list[0].address == Address(40));
    CHECK(list[0].status.usb_status == "Disconnected");
    CHECK(list[0].online);
}
