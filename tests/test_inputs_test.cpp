// Checks the test harness's input names (src/Test/test_inputs.cpp): which input
// each name drives on each instrument.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Test/test_inputs.h"

using namespace band3::test;
using band3::input::InstrumentInputs;
using band3::input::InstrumentKind;
namespace input = band3::input;

TEST_CASE("a guitar fret is pressed and let go by name") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kGuitar, in, "green", 100) == "");
    CHECK(in.guitar.frets[input::kGreen]);
    CHECK(SetInput(InstrumentKind::kGuitar, in, "green", 0) == "");
    CHECK_FALSE(in.guitar.frets[input::kGreen]);
}

TEST_CASE("guitar strums and solo are named inputs") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kGuitar, in, "strum_down", 100) == "");
    CHECK(SetInput(InstrumentKind::kGuitar, in, "solo", 100) == "");
    CHECK(in.guitar.strum_down);
    CHECK(in.guitar.solo);
}

TEST_CASE("the buttons every instrument has drive that instrument's nav") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kDrums, in, "start", 100) == "");
    CHECK(SetInput(InstrumentKind::kDrums, in, "up", 100) == "");
    CHECK(in.drums.nav.start);
    CHECK(in.drums.nav.dpad_up);
    CHECK_FALSE(in.guitar.nav.start);

    CHECK(SetInput(InstrumentKind::kProGuitarSquier, in, "a", 100) == "");
    CHECK(in.pro_guitar.nav.a);
}

TEST_CASE("an input the instrument doesn't have is an error naming both") {
    InstrumentInputs in;
    const std::string error = SetInput(InstrumentKind::kGuitar, in, "red_pad", 100);
    CHECK(error.find("red_pad") != std::string::npos);
    CHECK(error.find("guitar") != std::string::npos);
    CHECK(SetInput(InstrumentKind::kGuitar, in, "nonsense", 100) != "");
}

TEST_CASE("drum pads and cymbals take the value as their velocity") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kDrums, in, "red_pad", 90) == "");
    CHECK(SetInput(InstrumentKind::kDrums, in, "blue_cym", 40) == "");
    CHECK(SetInput(InstrumentKind::kDrums, in, "kick", 100) == "");
    CHECK(in.drums.pads[input::kRedPad] == 90);
    CHECK(in.drums.cymbals[input::kBlueCymbal] == 40);
    CHECK(in.drums.kick1);
}

TEST_CASE("keys are key0 to key24 and take a velocity") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kKeys, in, "key12", 70) == "");
    CHECK(in.keys.keys[12] == 70);
    CHECK(SetInput(InstrumentKind::kKeys, in, "key24", 1) == "");
    CHECK(in.keys.keys[24] == 1);
    CHECK(SetInput(InstrumentKind::kKeys, in, "key25", 100) != "");
    CHECK(SetInput(InstrumentKind::kKeys, in, "key", 100) != "");
    CHECK(SetInput(InstrumentKind::kKeys, in, "overdrive", 100) == "");
    CHECK(in.keys.overdrive);
}

TEST_CASE("a pro guitar string sounds at a fret") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kProGuitarMustang, in, "low_e", 100, 5) == "");
    CHECK(in.pro_guitar.frets[input::kStringLowE] == 5);
    CHECK(in.pro_guitar.velocities[input::kStringLowE] == 100);

    // letting go leaves the string silent and open
    CHECK(SetInput(InstrumentKind::kProGuitarMustang, in, "low_e", 0) == "");
    CHECK(in.pro_guitar.frets[input::kStringLowE] == 0);
    CHECK(in.pro_guitar.velocities[input::kStringLowE] == 0);

    CHECK(SetInput(InstrumentKind::kProGuitarSquier, in, "high_e", 100, 23) != "");
}

TEST_CASE("pro guitar strings don't take the a and b buttons' names") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kProGuitarSquier, in, "a_str", 80, 2) == "");
    CHECK(in.pro_guitar.frets[input::kStringA] == 2);
    CHECK(SetInput(InstrumentKind::kProGuitarSquier, in, "b", 100) == "");
    CHECK(in.pro_guitar.nav.b);
    CHECK(in.pro_guitar.velocities[input::kStringB] == 0);
}

TEST_CASE("a fret is only for pro guitar strings") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kGuitar, in, "green", 100, 3) != "");
}

TEST_CASE("pro guitars press their 5-fret colors by name") {
    InstrumentInputs in;
    CHECK(SetInput(InstrumentKind::kProGuitarSquier, in, "orange", 100) == "");
    CHECK(in.pro_guitar.colors[input::kOrange]);
}

TEST_CASE("whammy and tilt are guitar axes from 0 to 1") {
    InstrumentInputs in;
    CHECK(SetAxis(InstrumentKind::kGuitar, in, "whammy", 0.5f) == "");
    CHECK(in.guitar.whammy == doctest::Approx(0.5f));
    CHECK(SetAxis(InstrumentKind::kGuitar, in, "tilt", 1.0f) == "");
    CHECK(in.guitar.tilt == doctest::Approx(1.0f));

    CHECK(SetAxis(InstrumentKind::kGuitar, in, "whammy", 1.5f) != "");
    CHECK(SetAxis(InstrumentKind::kDrums, in, "whammy", 0.5f) != "");
    CHECK(SetAxis(InstrumentKind::kGuitar, in, "green", 0.5f) != "");
}

TEST_CASE("inputs pressed together are joined with +") {
    CHECK(SplitInputs("green") == std::vector<std::string>{"green"});
    CHECK(SplitInputs("green+red+strum_down") ==
          std::vector<std::string>{"green", "red", "strum_down"});
    CHECK(SplitInputs("green++red") == std::vector<std::string>{"green", "", "red"});
}

TEST_CASE("instruments have short names for the instrument command") {
    CHECK(ParseInstrumentName("guitar") == InstrumentKind::kGuitar);
    CHECK(ParseInstrumentName("drums") == InstrumentKind::kDrums);
    CHECK(ParseInstrumentName("keys") == InstrumentKind::kKeys);
    CHECK(ParseInstrumentName("mustang") == InstrumentKind::kProGuitarMustang);
    CHECK(ParseInstrumentName("squier") == InstrumentKind::kProGuitarSquier);
    CHECK_FALSE(ParseInstrumentName("banjo").has_value());
    CHECK(std::string(InstrumentName(InstrumentKind::kProGuitarSquier)) == "squier");
}
