// Checks the Steam Deck hotspot's plan (src/Lights/hotspot_plan.cpp): what
// its name and password may be, the nmcli commands that make and start it,
// and reading what nmcli prints; and which networks' broadcasts reach the
// devices that join it (src/Net/broadcast_addresses.cpp).

#include <doctest/doctest.h>
#include <algorithm>
#include "src/Lights/hotspot_plan.h"
#include "src/Net/broadcast_addresses.h"

using namespace band3::lights::hotspot;

namespace {

bool Has(const std::vector<std::string>& args, std::string_view key, std::string_view value) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == key && args[i + 1] == value) return true;
    }
    return false;
}

// a.b.c.d in network order (its bytes in address order)
uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    return uint32_t(a) | uint32_t(b) << 8 | uint32_t(c) << 16 | uint32_t(d) << 24;
}

}

TEST_CASE("a hotspot's name is 1 to 32 bytes, its password 8 to 63 printable characters") {
    CHECK(ValidName("band3"));
    CHECK(ValidName(std::string(32, 'x')));
    CHECK_FALSE(ValidName(""));
    CHECK_FALSE(ValidName(std::string(33, 'x')));

    CHECK(ValidPassword("12345678"));
    CHECK(ValidPassword("stage kit! #1"));
    CHECK(ValidPassword(std::string(63, 'x')));
    CHECK_FALSE(ValidPassword("1234567"));
    CHECK_FALSE(ValidPassword(std::string(64, 'x')));
    CHECK_FALSE(ValidPassword("tab\there1"));
    CHECK_FALSE(ValidPassword("caf\xc3\xa9 rock"));

    CHECK(ConfigProblem({"band3", "rockband3"}).empty());
    CHECK(ConfigProblem({"band3", ""}).find("Set a password") == 0);
    CHECK_FALSE(ConfigProblem({"band3", "short"}).empty());
    CHECK_FALSE(ConfigProblem({"", "rockband3"}).empty());
}

TEST_CASE("the hotspot is a 2.4 GHz WPA2 network band3 starts itself, with DHCP where it can") {
    const Config config{"Stage: lights", "rockband3"};
    for (const auto& args : {AddArgs(config, "wlan0", Addressing::kDhcp),
                             ModifyArgs(config, Addressing::kDhcp)}) {
        CHECK(Has(args, "802-11-wireless.ssid", "Stage: lights"));
        CHECK(Has(args, "802-11-wireless.mode", "ap"));
        CHECK(Has(args, "802-11-wireless.band", "bg"));
        CHECK(Has(args, "ipv4.method", "shared"));
        CHECK(Has(args, "connection.autoconnect", "no"));
        CHECK(Has(args, "wifi-sec.key-mgmt", "wpa-psk"));
        CHECK(Has(args, "wifi-sec.psk", "rockband3"));
        CHECK(Has(args, "wifi-sec.pmf", "disable"));
    }
    const auto add = AddArgs(config, "wlan0", Addressing::kDhcp);
    CHECK(Has(add, "con-name", kConnection));
    CHECK(Has(add, "ifname", "wlan0"));
    CHECK(Has(ModifyArgs(config, Addressing::kDhcp), "modify", kConnection));
    // without dnsmasq (SteamOS): link-local, which needs no DHCP server
    CHECK(Has(AddArgs(config, "wlan0", Addressing::kLinkLocal), "ipv4.method", "link-local"));
    CHECK(Has(ModifyArgs(config, Addressing::kLinkLocal), "ipv4.method", "link-local"));
    CHECK(Has(UpArgs("wlan0"), "up", kConnection));
    CHECK(Has(UpArgs("wlan0"), "ifname", "wlan0"));
    CHECK(Has(DownArgs(), "down", kConnection));
}

TEST_CASE("nmcli's terse lines split at colons that aren't escaped") {
    CHECK(SplitTerse("wlan0:wifi:connected") == std::vector<std::string>{"wlan0", "wifi", "connected"});
    CHECK(SplitTerse("a\\:b:c\\\\d") == std::vector<std::string>{"a:b", "c\\d"});
    CHECK(SplitTerse("") == std::vector<std::string>{""});
    CHECK(SplitTerse("x:") == std::vector<std::string>{"x", ""});
}

TEST_CASE("the Wi-Fi device is the first of nmcli's devices that's Wi-Fi") {
    const std::vector<std::string> lines = {
        "enp4s0f3u1u4c2:ethernet:connected",
        "wlan0:wifi:connected",
        "p2p-dev-wlan0:wifi-p2p:disconnected",
        "lo:loopback:connected (externally)",
    };
    const auto device = FindWifiDevice(lines);
    REQUIRE(device);
    CHECK(device->name == "wlan0");
    CHECK(device->available);

    const auto off = FindWifiDevice({"wlan0:wifi:unavailable"});
    REQUIRE(off);
    CHECK_FALSE(off->available);

    CHECK_FALSE(FindWifiDevice({"eth0:ethernet:connected", "p2p-dev-wlan0:wifi-p2p:disconnected"}));
    CHECK_FALSE(FindWifiDevice({}));
}

TEST_CASE("the active connections list the hotspot by its name alone") {
    CHECK(ListsConnection({"Wired connection 1", "band3 hotspot", "lo"}));
    CHECK_FALSE(ListsConnection({"HamHaus_5G", "lo"}));
    CHECK_FALSE(ListsConnection({"band3 hotspot 2"}));
    CHECK_FALSE(ListsConnection({}));
}

TEST_CASE("the Deck's address on the hotspot, and what nmcli said went wrong") {
    CHECK(FirstAddress({"10.42.0.1/24"}) == "10.42.0.1");
    CHECK(FirstAddress({"10.42.0.1/24 | 192.168.7.2/24"}) == "10.42.0.1");
    CHECK(FirstAddress({"", "10.42.0.1/24"}) == "10.42.0.1");
    CHECK(FirstAddress({}).empty());

    CHECK(ErrorText({"Error: Connection activation failed: No suitable device found.", "Hint: ..."}) ==
          "Connection activation failed: No suitable device found.");
    CHECK(ErrorText({"something", "last words"}) == "last words");
    CHECK(ErrorText({}).empty());
}

TEST_CASE("broadcasts go to the networks the default route doesn't reach, each once") {
    using band3::net::Interface;
    const uint32_t mask24 = Ip(255, 255, 255, 0);
    const Interface interfaces[] = {
        {.address = Ip(127, 0, 0, 1), .netmask = Ip(255, 0, 0, 0), .up = true, .loopback = true},
        // the default route's
        {.address = Ip(192, 168, 50, 220), .netmask = mask24, .up = true, .broadcast = true},
        // the hotspot
        {.address = Ip(10, 42, 0, 1), .netmask = mask24, .up = true, .broadcast = true},
        // a second address on it
        {.address = Ip(10, 42, 0, 2), .netmask = mask24, .up = true, .broadcast = true},
        // down, and a point-to-point link (WireGuard)
        {.address = Ip(10, 9, 0, 1), .netmask = mask24, .up = false, .broadcast = true},
        {.address = Ip(10, 8, 0, 2), .netmask = mask24, .up = true, .broadcast = false},
    };
    CHECK(band3::net::OtherBroadcasts(interfaces, Ip(192, 168, 50, 220)) ==
          std::vector<uint32_t>{Ip(10, 42, 0, 255)});
    // no default route (the hotspot alone): every network's
    CHECK(band3::net::OtherBroadcasts(interfaces, 0) ==
          std::vector<uint32_t>{Ip(192, 168, 50, 255), Ip(10, 42, 0, 255)});
    const Interface wide{.address = Ip(10, 1, 2, 3), .netmask = Ip(255, 255, 0, 0), .up = true,
                         .broadcast = true};
    CHECK(band3::net::OtherBroadcasts({&wide, 1}, 0) == std::vector<uint32_t>{Ip(10, 1, 255, 255)});
}
