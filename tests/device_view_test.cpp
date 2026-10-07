// Checks the pure parts of the launcher's Controllers device list, test view
// and gamepad navigation (src/Launcher/device_view_model.cpp): the kinds'
// labels, which view draws a device, how a hit fades, and what a device's
// state does to navigation, and the test mode that keeps a tested device out
// of it.

#include <doctest/doctest.h>
#include <cstdint>
#include <vector>
#include "src/Input/instruments.h"
#include "src/Launcher/device_view_model.h"

using namespace band3::launcher;
using band3::input::DeviceKind;
namespace input = band3::input;
namespace xbox = band3::input::xbox;

TEST_CASE("devices are labelled by kind, and pads and instruments by the subtype they report") {
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", std::nullopt) == "Controller");
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", input::kSubtypeGamepad) == "Controller");
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", input::kSubtypeGuitar) == "Xbox 360 guitar");
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", input::kSubtypeGuitarBass) == "Xbox 360 guitar");
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", input::kSubtypeDrums) == "Xbox 360 drum kit");
    CHECK(DeviceKindLabel(DeviceKind::kPad, "Pad", input::kSubtypeKeytar) == "Xbox 360 keytar");
    CHECK(DeviceKindLabel(DeviceKind::kSynthetic, "Keyboard and Mouse", input::kSubtypeGamepad) ==
          "Keyboard and mouse");
    CHECK(DeviceKindLabel(DeviceKind::kSynthetic, "None", input::kSubtypeGamepad) ==
          "Stand-in (presses nothing)");
    CHECK(DeviceKindLabel(DeviceKind::kVirtual, "Pad", input::kSubtypeDrums) ==
          "Virtual drum kit (debug)");
    CHECK(DeviceKindLabel(DeviceKind::kVirtual, "Pad", std::nullopt) == "Virtual instrument (debug)");
    CHECK(DeviceKindLabel(DeviceKind::kHidInstrument, "Pad", input::kSubtypeGuitar) ==
          "Guitar (USB dongle)");
    CHECK(DeviceKindLabel(DeviceKind::kHidInstrument, "Pad", std::nullopt) == "Instrument (USB dongle)");
    CHECK(DeviceKindLabel(DeviceKind::kMidiDrums, "Pad", input::kSubtypeDrums) == "MIDI drum kit");
    CHECK(DeviceKindLabel(DeviceKind::kMidiKeys, "Pad", input::kSubtypeKeytar) == "MIDI keyboard");
    CHECK(DeviceKindLabel(DeviceKind::kSdlCopy, "Pad", std::nullopt).find("unused") != std::string::npos);
}

TEST_CASE("the SDK's stand-in is told from the keyboard by name") {
    // the names the SDK's drivers give them (rexruntime.dll): NopInputDriver's
    // stand-in, and MnkInputDriver's keyboard in its two modes
    CHECK(IsStandIn(DeviceKind::kSynthetic, "None"));
    CHECK_FALSE(IsStandIn(DeviceKind::kSynthetic, "Keyboard and Mouse"));
    CHECK_FALSE(IsStandIn(DeviceKind::kSynthetic, "Keyboard"));
    CHECK(DeviceKindLabel(DeviceKind::kSynthetic, "Keyboard", std::nullopt) ==
          "Keyboard and mouse");
    CHECK_FALSE(IsStandIn(DeviceKind::kSynthetic, "mouse"));
    CHECK_FALSE(IsStandIn(DeviceKind::kPad, "None"));
}

TEST_CASE("players are named, and a device feeding none isn't playing") {
    CHECK(PlayerLabel(1) == "Player 1");
    CHECK(PlayerLabel(4) == "Player 4");
    CHECK(PlayerLabel(0) == "Not playing");
}

TEST_CASE("controller_type names what gamepads play as") {
    CHECK(PlaysAsLabel(7) == "Guitar");
    CHECK(PlaysAsLabel(8) == "Drums");
    CHECK(PlaysAsLabel(1) == "Vocals");
    CHECK_FALSE(PlaysAsLabel(-1).has_value());
    CHECK(PlaysAsLabel(5) == "instrument type 5");
}

TEST_CASE("the test view follows the subtype a device reports") {
    CHECK(ViewFor(input::GuitarCaps()) == TestView::kGuitar);
    CHECK(ViewFor(input::GuitarCaps(false)) == TestView::kGuitar);
    CHECK(ViewFor(input::DrumCaps()) == TestView::kDrums);
    CHECK(ViewFor(input::DrumCaps(false)) == TestView::kDrums);
    CHECK(ViewFor(input::Caps360{}) == TestView::kPad);
    CHECK(ViewFor(input::KeysCaps()) == TestView::kKeys);
    CHECK(ViewFor(input::ProGuitarCaps(input::ProGuitarModel::kSquier)) == TestView::kPad);
}

TEST_CASE("a hit flashes as brightly as it was hit, and fades out") {
    CHECK(FlashLevel(0, 0.0f) == 0.0f);
    CHECK(FlashLevel(127, 0.0f) == doctest::Approx(1.0f));
    CHECK(FlashLevel(1, 0.0f) == doctest::Approx(0.35f));
    CHECK(FlashLevel(64, 0.0f) > FlashLevel(32, 0.0f));
    CHECK(FlashLevel(127, kFlashSeconds / 2) == doctest::Approx(0.5f));
    CHECK(FlashLevel(127, kFlashSeconds) == 0.0f);
    CHECK(FlashLevel(127, 10.0f) == 0.0f);
}

TEST_CASE("sticks and triggers count past XInput's dead zones") {
    CHECK(StickAmount(0) == 0.0f);
    CHECK(StickAmount(7000) == 0.0f);
    CHECK(StickAmount(-7000) == 0.0f);
    CHECK(StickAmount(32767) == doctest::Approx(1.0f));
    CHECK(StickAmount(-32768) == doctest::Approx(-1.0f));
    CHECK(StickAmount(20000) > 0.0f);
    CHECK(StickAmount(-20000) < 0.0f);
    CHECK(TriggerAmount(0) == 0.0f);
    CHECK(TriggerAmount(30) == 0.0f);
    CHECK(TriggerAmount(255) == doctest::Approx(1.0f));
}

TEST_CASE("the keyboard and SDL's copies of dongle instruments don't navigate") {
    CHECK(DrivesNavigation(DeviceKind::kPad));
    CHECK(DrivesNavigation(DeviceKind::kVirtual));
    CHECK(DrivesNavigation(DeviceKind::kHidInstrument));
    CHECK(DrivesNavigation(DeviceKind::kMidiDrums));
    CHECK(DrivesNavigation(DeviceKind::kMidiKeys));
    CHECK_FALSE(DrivesNavigation(DeviceKind::kSynthetic));
    CHECK_FALSE(DrivesNavigation(DeviceKind::kSdlCopy));
}

TEST_CASE("a gamepad navigates with everything it has") {
    input::Gamepad360 g;
    g.buttons = xbox::kButtonA | xbox::kLeftShoulder | xbox::kDpadUp;
    g.thumb_ly = 32767;
    g.thumb_rx = -32768;
    g.right_trigger = 255;
    const NavPad pad = NavFromReading(input::Caps360{}, g);
    CHECK(pad.buttons == g.buttons);
    CHECK(pad.left_y == doctest::Approx(1.0f));
    CHECK(pad.right_x == doctest::Approx(-1.0f));
    CHECK(pad.right_trigger == doctest::Approx(1.0f));
    CHECK(pad.left_x == 0.0f);
}

TEST_CASE("a guitar navigates with its menu buttons only") {
    input::GuitarInputs in;
    in.frets[input::kGreen] = true;
    in.frets[input::kOrange] = true;  // LB
    in.solo = true;                   // the left stick's button
    in.strum_down = true;
    in.whammy = 1.0f;  // the right stick
    in.tilt = 1.0f;
    in.pickup = 200;  // the left trigger
    const NavPad pad = NavFromReading(input::GuitarCaps(), input::EncodeGuitar(in));
    CHECK(pad.buttons == (xbox::kButtonA | xbox::kDpadDown));
    CHECK(pad == NavPad{.buttons = pad.buttons});
}

TEST_CASE("a drum kit's cymbal markers and kick pedal don't navigate") {
    input::DrumInputs in;
    in.cymbals[input::kYellowCymbal] = 100;  // Y with d-pad up
    in.kick1 = true;                         // LB
    NavPad pad = NavFromReading(input::DrumCaps(), input::EncodeDrums(in));
    CHECK(pad.buttons == xbox::kButtonY);

    // a pad hit is its face button
    in = {};
    in.pads[input::kGreenPad] = 90;
    pad = NavFromReading(input::DrumCaps(), input::EncodeDrums(in));
    CHECK(pad.buttons == xbox::kButtonA);

    // the d-pad navigates when no cymbal is hit
    in = {};
    in.nav.dpad_up = true;
    pad = NavFromReading(input::DrumCaps(), input::EncodeDrums(in));
    CHECK(pad.buttons == xbox::kDpadUp);

    // an RB1 kit has no cymbal flag, so its d-pad is always the d-pad
    input::Gamepad360 g;
    g.buttons = xbox::kDpadDown | xbox::kButtonB;
    pad = NavFromReading(input::DrumCaps(false), g);
    CHECK(pad.buttons == (xbox::kDpadDown | xbox::kButtonB));
}

TEST_CASE("devices combine: any device's buttons, the farthest push of each axis") {
    const std::vector<NavPad> pads = {
        {.buttons = xbox::kButtonA, .left_x = 0.5f, .left_trigger = 0.2f},
        {.buttons = xbox::kStart, .left_x = -0.8f, .left_trigger = 0.1f},
    };
    const NavPad all = CombinePads(pads);
    CHECK(all.buttons == (xbox::kButtonA | xbox::kStart));
    CHECK(all.left_x == doctest::Approx(-0.8f));
    CHECK(all.left_trigger == doctest::Approx(0.2f));
    CHECK(CombinePads({}) == NavPad{});
}

TEST_CASE("Start and the bumpers count once, as they go down") {
    NavEdges e = PressedEdges(0, xbox::kStart | xbox::kRightShoulder);
    CHECK(e.start);
    CHECK(e.tab_next);
    CHECK_FALSE(e.tab_previous);
    e = PressedEdges(xbox::kStart | xbox::kRightShoulder, xbox::kStart | xbox::kRightShoulder);
    CHECK_FALSE(e.start);
    CHECK_FALSE(e.tab_next);
    e = PressedEdges(xbox::kStart, xbox::kLeftShoulder);
    CHECK(e.tab_previous);
    CHECK_FALSE(e.start);
}

TEST_CASE("outside test mode every device navigates") {
    TestMode test;
    const NavPad a{.buttons = xbox::kButtonA, .left_x = 0.5f};
    CHECK(test.Filter(1, a) == a);
    CHECK(test.Filter(2, a) == a);
    CHECK_FALSE(test.Testing().has_value());
}

TEST_CASE("the device under test doesn't navigate, and the others still do") {
    TestMode test;
    test.Begin(1);
    CHECK(test.Testing() == 1u);
    const NavPad frets{.buttons = xbox::kButtonA | xbox::kButtonB | xbox::kStart, .right_x = 1.0f};
    CHECK(test.Filter(1, frets) == NavPad{});
    CHECK(test.Filter(1, frets) == NavPad{});
    CHECK(test.Filter(2, frets) == frets);
    // another device's Back doesn't end it
    CHECK(test.Filter(2, {.buttons = xbox::kBack}).buttons == xbox::kBack);
    CHECK(test.Testing() == 1u);
}

TEST_CASE("Back on the device under test ends the test, and what it holds counts once let go") {
    TestMode test;
    test.Begin(1);
    test.Filter(1, {.buttons = xbox::kButtonA});
    // Back pressed with the green fret held
    CHECK(test.Filter(1, {.buttons = xbox::kButtonA | xbox::kBack}) == NavPad{});
    CHECK_FALSE(test.Testing().has_value());
    // the fret and Back still held don't navigate; a new button and the sticks do
    NavPad pad = test.Filter(1, {.buttons = xbox::kButtonA | xbox::kBack | xbox::kDpadDown,
                                 .left_y = -1.0f});
    CHECK(pad.buttons == xbox::kDpadDown);
    CHECK(pad.left_y == doctest::Approx(-1.0f));
    // Back let go, the fret still held
    CHECK(test.Filter(1, {.buttons = xbox::kButtonA}).buttons == 0);
    // the fret let go, then pressed again
    CHECK(test.Filter(1, {}).buttons == 0);
    CHECK(test.Filter(1, {.buttons = xbox::kButtonA}).buttons == xbox::kButtonA);
    CHECK(test.Filter(1, {.buttons = xbox::kBack}).buttons == xbox::kBack);
}

TEST_CASE("a Back held as the test begins doesn't end it until pressed again") {
    TestMode test;
    test.Begin(1);
    test.Filter(1, {.buttons = xbox::kBack});
    test.Filter(1, {.buttons = xbox::kBack});
    CHECK(test.Testing() == 1u);
    test.Filter(1, {});
    test.Filter(1, {.buttons = xbox::kBack});
    CHECK_FALSE(test.Testing().has_value());
}

TEST_CASE("Stop ends the test, and what the device holds counts once let go") {
    TestMode test;
    test.Begin(1);
    test.Filter(1, {.buttons = xbox::kButtonB});
    test.End();
    CHECK_FALSE(test.Testing().has_value());
    CHECK(test.Filter(1, {.buttons = xbox::kButtonB}).buttons == 0);
    CHECK(test.Filter(1, {}).buttons == 0);
    CHECK(test.Filter(1, {.buttons = xbox::kButtonB}).buttons == xbox::kButtonB);
}

TEST_CASE("testing another device ends the first one's test") {
    TestMode test;
    test.Begin(1);
    test.Filter(1, {.buttons = xbox::kButtonY});
    test.Begin(2);
    CHECK(test.Testing() == 2u);
    CHECK(test.Filter(2, {.buttons = xbox::kButtonA}) == NavPad{});
    CHECK(test.Filter(1, {.buttons = xbox::kButtonY}).buttons == 0);
    CHECK(test.Filter(1, {.buttons = xbox::kButtonA}).buttons == xbox::kButtonA);
    // testing the same device again changes nothing
    test.Begin(2);
    CHECK(test.Testing() == 2u);
}

TEST_CASE("the test ends when its device goes or the Controllers tab stops showing") {
    TestMode test;
    const std::vector<uint64_t> both = {1, 2};
    const std::vector<uint64_t> other = {2};
    test.Begin(1);
    test.Follow(both, true);
    CHECK(test.Testing() == 1u);
    test.Follow(other, true);
    CHECK_FALSE(test.Testing().has_value());

    test.Begin(1);
    test.Follow(both, false);
    CHECK_FALSE(test.Testing().has_value());

    // nothing to end
    test.Follow({}, false);
    CHECK_FALSE(test.Testing().has_value());
}
