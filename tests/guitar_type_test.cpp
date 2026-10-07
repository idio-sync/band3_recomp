// Checks which guitar RB3 is told a guitar is (src/Input/guitar_type.h): the
// guitar_type values, the XInput subtype SDL keeps in a joystick GUID, and the
// subtype RB3 reads for each setting.

#include <doctest/doctest.h>
#include <cstdint>
#include <optional>
#include <ostream>
#include "src/Input/guitar_type.h"
#include "src/Input/instruments.h"

using namespace band3::input;

TEST_CASE("guitar_type's values") {
    CHECK(ParseGuitarType("auto") == GuitarType::kAuto);
    CHECK(ParseGuitarType("rock_band") == GuitarType::kRockBand);
    CHECK(ParseGuitarType("guitar_hero") == GuitarType::kGuitarHero);
    CHECK(ParseGuitarType("") == std::nullopt);
    CHECK(ParseGuitarType("Guitar_Hero") == std::nullopt);
}

TEST_CASE("SDL's XInput GUIDs carry the device's own subtype") {
    // a wired Guitar Hero X-plorer (1430:4748), as SDL 3.4 logged it: 'x' (0x78)
    // in byte 14, GuitarAlternate in byte 15
    CHECK(XInputSubtypeFromGuid("0300cd7e301400004847000022317807") == kSubtypeGuitarAlternate);
    // a Rock Band 2 drum kit through the same backend
    CHECK(XInputSubtypeFromGuid("0300eebfad1b00000300000002017808") == kSubtypeDrums);
    // upper case hex reads the same
    CHECK(XInputSubtypeFromGuid("0300CD7E301400004847000022317806") == kSubtypeGuitar);
}

TEST_CASE("GUIDs from other backends, and anything else, carry no subtype") {
    // RAWINPUT ('r') and HIDAPI ('h') put their own byte there
    CHECK(XInputSubtypeFromGuid("0300cd7e301400004847000022317207") == std::nullopt);
    CHECK(XInputSubtypeFromGuid("0300cd7e301400004847000022316807") == std::nullopt);
    // Linux evdev leaves bytes 14 and 15 zero
    CHECK(XInputSubtypeFromGuid("030000004c0500006802000011810000") == std::nullopt);
    // band3's own drivers' GUIDs, and broken ones
    CHECK(XInputSubtypeFromGuid("virtual-instrument-2") == std::nullopt);
    CHECK(XInputSubtypeFromGuid("") == std::nullopt);
    CHECK(XInputSubtypeFromGuid("0300cd7e30140000484700002231780") == std::nullopt);
    CHECK(XInputSubtypeFromGuid("0300cd7e30140000484700002231780g") == std::nullopt);
    CHECK(XInputSubtypeFromGuid("0300cd7e3014000048470000223178zz") == std::nullopt);
}

TEST_CASE("guitars are 6, 7 and 11") {
    CHECK(IsGuitarSubtype(kSubtypeGuitar));
    CHECK(IsGuitarSubtype(kSubtypeGuitarAlternate));
    CHECK(IsGuitarSubtype(kSubtypeGuitarBass));
    CHECK_FALSE(IsGuitarSubtype(kSubtypeGamepad));
    CHECK_FALSE(IsGuitarSubtype(kSubtypeDrums));
    CHECK_FALSE(IsGuitarSubtype(kSubtypeKeytar));
    CHECK_FALSE(IsGuitarSubtype(kSubtypeProGuitar));
}

TEST_CASE("auto gives a guitar the subtype it reports itself") {
    // the bug: SDL's backend reports every guitar as 6, so a Guitar Hero guitar's
    // tilt sensor (its left trigger) worked RB3's effect switch
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitar, kSubtypeGuitarAlternate) ==
          kSubtypeGuitarAlternate);
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitar, kSubtypeGuitar) == kSubtypeGuitar);
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitar, kSubtypeGuitarBass) ==
          kSubtypeGuitarBass);
    // unknown (Linux, a PlayStation guitar, the XInput backend): as reported
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitar, std::nullopt) == kSubtypeGuitar);
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitarAlternate, std::nullopt) ==
          kSubtypeGuitarAlternate);
    // a device whose own subtype isn't a guitar doesn't make the reported guitar one
    CHECK(GuitarSubtypeFor(GuitarType::kAuto, kSubtypeGuitar, kSubtypeGamepad) == kSubtypeGuitar);
}

TEST_CASE("Guitar Hero makes every guitar 7, and Rock Band every 7 a 6") {
    for (const auto native : {std::optional<uint8_t>{}, std::optional<uint8_t>{kSubtypeGuitar},
                              std::optional<uint8_t>{kSubtypeGuitarAlternate}}) {
        CHECK(GuitarSubtypeFor(GuitarType::kGuitarHero, kSubtypeGuitar, native) ==
              kSubtypeGuitarAlternate);
        CHECK(GuitarSubtypeFor(GuitarType::kGuitarHero, kSubtypeGuitarBass, native) ==
              kSubtypeGuitarAlternate);
        CHECK(GuitarSubtypeFor(GuitarType::kRockBand, kSubtypeGuitarAlternate, native) ==
              kSubtypeGuitar);
        CHECK(GuitarSubtypeFor(GuitarType::kRockBand, kSubtypeGuitar, native) == kSubtypeGuitar);
        // a bass stays a bass: RB3 reads 11 as it reads 6
        CHECK(GuitarSubtypeFor(GuitarType::kRockBand, kSubtypeGuitarBass, native) ==
              kSubtypeGuitarBass);
    }
}

TEST_CASE("only guitars change") {
    for (const auto type : {GuitarType::kAuto, GuitarType::kRockBand, GuitarType::kGuitarHero}) {
        for (const uint8_t subtype : {kSubtypeGamepad, kSubtypeDrums, kSubtypeKeytar,
                                      kSubtypeProGuitar, uint8_t{9}}) {
            CHECK(GuitarSubtypeFor(type, subtype, kSubtypeGuitarAlternate) == subtype);
        }
    }
}
