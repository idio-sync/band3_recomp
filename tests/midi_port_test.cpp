// Checks which MIDI input port band3's MIDI devices open
// (src/Input/midi_port_select.cpp): a named port by part of its name, any case,
// or with none named the first that isn't a loopback or another device's.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Input/midi_port_select.h"

using namespace band3::input;

namespace {

const std::vector<std::string> kLinuxPorts = {"Midi Through:Midi Through Port-0 14:0",
                                              "TD-17:TD-17 MIDI 1 20:0",
                                              "Keystation 49 MK3:Keystation 49 MK3 MIDI 1 24:0"};

}

TEST_CASE("a named port is the first whose name contains it, in any case") {
    CHECK(PickPort(kLinuxPorts, "td-17", {}) == 1u);
    CHECK(PickPort(kLinuxPorts, "KEYSTATION", {}) == 2u);
    CHECK(PickPort(kLinuxPorts, "MIDI 1", {}) == 1u);
    // the player named it: the loopback too
    CHECK(PickPort(kLinuxPorts, "through", {}) == 0u);
    CHECK_FALSE(PickPort(kLinuxPorts, "Alesis", {}).has_value());
}

TEST_CASE("a named port is opened even when another band3 MIDI device holds it") {
    CHECK(PickPort(kLinuxPorts, "td-17", {kLinuxPorts[1]}) == 1u);
    CHECK(PickPort(kLinuxPorts, "MIDI 1", {kLinuxPorts[1]}) == 1u);
}

TEST_CASE("with no port named, the first that isn't a loopback or taken") {
    CHECK(PickPort(kLinuxPorts, "", {}) == 1u);
    // a drum kit holds the first: the keyboard takes the next
    CHECK(PickPort(kLinuxPorts, "", {kLinuxPorts[1]}) == 2u);
    // one that isn't listed takes nothing
    CHECK(PickPort(kLinuxPorts, "", {"Alesis Nitro 0"}) == 1u);
    const std::vector<std::string> windows_ports = {"TD-17 0", "USB MIDI Interface 1"};
    CHECK(PickPort(windows_ports, "", {}) == 0u);
    CHECK(PickPort(windows_ports, "", {"TD-17 0"}) == 1u);
}

TEST_CASE("nothing suitable picks nothing") {
    CHECK_FALSE(PickPort({"Midi Through:Midi Through Port-0 14:0"}, "", {}).has_value());
    CHECK_FALSE(PickPort(kLinuxPorts, "", {kLinuxPorts[1], kLinuxPorts[2]}).has_value());
    CHECK_FALSE(PickPort({}, "", {}).has_value());
    CHECK_FALSE(PickPort({}, "TD-17", {}).has_value());
}
