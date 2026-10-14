// Checks how band3 finds an SDL-read Xbox 360 drum kit's own capabilities
// among Windows' XInput pads (src/Input/xinput_caps.h): the ids SDL keeps in
// the GUID, and which pads' capabilities it takes.

#include <doctest/doctest.h>
#include <cstdint>
#include <optional>
#include <ostream>
#include <vector>
#include "src/Input/instruments.h"
#include "src/Input/xinput_caps.h"

using namespace band3::input;

namespace {

// an ION Drum Rocker as PlasticBand's Xbox 360 drum doc gives its capabilities:
// wired, so no flags, and its hardware ids in the sticks
Caps360 DrumRockerCaps() {
    Caps360 c;
    c.sub_type = kSubtypeDrums;
    c.gamepad.buttons = 0xF3FF;
    c.gamepad.left_trigger = 0xFF;
    c.gamepad.right_trigger = 0xFF;
    c.gamepad.thumb_lx = 0x15E4;
    c.gamepad.thumb_ly = 0x0130;
    c.gamepad.thumb_rx = 0x0100;
    return c;
}

// an RB1 kit: all-zero trigger and stick capabilities
Caps360 Rb1Caps() {
    Caps360 c;
    c.sub_type = kSubtypeDrums;
    c.gamepad.buttons = 0xF3FF;
    return c;
}

Caps360 GamepadCaps() {
    Caps360 c;
    c.sub_type = kSubtypeGamepad;
    c.flags = 0x0001;
    c.gamepad.buttons = 0xF3FF;
    c.gamepad.thumb_lx = -64;
    return c;
}

}

TEST_CASE("SDL's XInput GUIDs carry the vendor, product and subtype") {
    // a Rock Band 2 drum kit (1BAD:0003) through SDL's XInput backend
    const auto ids = XInputIdsFromGuid("0300eebfad1b00000300000002017808");
    REQUIRE(ids);
    CHECK(ids->vendor == 0x1BAD);
    CHECK(ids->product == 0x0003);
    CHECK(ids->subtype == kSubtypeDrums);
    // upper case hex reads the same
    CHECK(XInputIdsFromGuid("0300EEBFAD1B00000300000002017808")->vendor == 0x1BAD);
}

TEST_CASE("a GUID SDL made from the name has no vendor or product") {
    // with no vendor SDL writes the device's name from byte 4 ("Contr")
    const auto ids = XInputIdsFromGuid("0300eebf436f6e74720000000000" "7808");
    REQUIRE(ids);
    CHECK(ids->vendor == 0);
    CHECK(ids->product == 0);
    CHECK(ids->subtype == kSubtypeDrums);
}

TEST_CASE("GUIDs from other backends, and anything else, carry no ids") {
    CHECK_FALSE(XInputIdsFromGuid("0300eebfad1b00000300000002017208"));  // RAWINPUT
    CHECK_FALSE(XInputIdsFromGuid("0300eebfad1b00000300000002016808"));  // HIDAPI
    CHECK_FALSE(XInputIdsFromGuid("virtual-instrument-2"));
    CHECK_FALSE(XInputIdsFromGuid(""));
    CHECK_FALSE(XInputIdsFromGuid("0300eebfad1b0000030000000201780"));
    CHECK_FALSE(XInputIdsFromGuid("0300eebfad1b000003000000020178zz"));
}

TEST_CASE("the kit's own capabilities come from the Windows pad that is it") {
    const XInputGuidIds rocker{0x15E4, 0x0130, kSubtypeDrums};
    const std::vector<WindowsPad> pads = {
        {0x045E, 0x028E, GamepadCaps()},
        {0x15E4, 0x0130, DrumRockerCaps()},
    };
    const auto caps = MatchWindowsCaps(rocker, pads);
    REQUIRE(caps);
    CHECK(*caps == DrumRockerCaps());
    // which RB3 reads as an RB2-or-later kit, where SDL's made-up ones read as RB1
    CHECK(IsRb2Drums(*caps));
    Caps360 sdl;
    sdl.sub_type = kSubtypeDrums;
    sdl.gamepad.thumb_rx = -1;
    CHECK_FALSE(IsRb2Drums(sdl));
}

TEST_CASE("an RB1 kit stays an RB1 kit") {
    const std::vector<WindowsPad> pads = {{0x1BAD, 0x0002, Rb1Caps()}};
    const auto caps = MatchWindowsCaps({0x1BAD, 0x0002, kSubtypeDrums}, pads);
    REQUIRE(caps);
    CHECK_FALSE(IsRb2Drums(*caps));
}

TEST_CASE("only pads of the kit's subtype, vendor and product match") {
    const std::vector<WindowsPad> pads = {
        {0x045E, 0x028E, GamepadCaps()},
        {0x1BAD, 0x0002, Rb1Caps()},
    };
    CHECK_FALSE(MatchWindowsCaps({0x15E4, 0x0130, kSubtypeDrums}, pads));
    CHECK_FALSE(MatchWindowsCaps({0x15E4, 0x0130, kSubtypeDrums}, {}));
}

TEST_CASE("without ids on one side, the subtype decides, if the pads agree") {
    // SDL without the ids: the one drum kit Windows has
    std::vector<WindowsPad> pads = {
        {0x045E, 0x028E, GamepadCaps()},
        {0x15E4, 0x0130, DrumRockerCaps()},
    };
    CHECK(MatchWindowsCaps({0, 0, kSubtypeDrums}, pads) == DrumRockerCaps());
    // Windows without them (no XInputGetCapabilitiesEx)
    pads[1].vendor = 0;
    pads[1].product = 0;
    CHECK(MatchWindowsCaps({0x15E4, 0x0130, kSubtypeDrums}, pads) == DrumRockerCaps());
    // two kits that report differently, and nothing to tell them apart by
    pads.push_back({0, 0, Rb1Caps()});
    CHECK_FALSE(MatchWindowsCaps({0x15E4, 0x0130, kSubtypeDrums}, pads));
}

TEST_CASE("two kits of one model are the same answer") {
    const std::vector<WindowsPad> pads = {
        {0x15E4, 0x0130, DrumRockerCaps()},
        {0x15E4, 0x0130, DrumRockerCaps()},
    };
    CHECK(MatchWindowsCaps({0x15E4, 0x0130, kSubtypeDrums}, pads) == DrumRockerCaps());
}
