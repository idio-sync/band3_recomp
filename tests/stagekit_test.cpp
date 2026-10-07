// Checks the Stage Kit's commands (src/Lights/stagekit.cpp): bringing a kit to
// the lights it should show, how each kind of kit is sent a command, which
// devices are Stage Kits, and the launcher's test scenes.

#include <doctest/doctest.h>
#include <vector>
#include "src/Lights/stagekit.h"

using namespace band3::lights;

namespace {

// every command KitSync sends until the kit shows what it should
std::vector<Command> Drain(KitSync& sync) {
    std::vector<Command> sent;
    while (auto command = sync.Next()) {
        sent.push_back(*command);
        sync.Sent(*command);
        REQUIRE(sent.size() < 32);
    }
    return sent;
}

}

TEST_CASE("a kit whose lights aren't known is sent all-off first") {
    KitSync sync;
    sync.wanted = ApplyStageKit({}, 0x0F, kRed);
    CHECK(Drain(sync) == std::vector<Command>{kAllOffCommand, {0x0F, kRed}});
    CHECK(sync.shown == sync.wanted);
}

TEST_CASE("a kit that shows its lights is sent nothing") {
    KitSync sync;
    sync.shown = StageKit{};
    CHECK_FALSE(sync.Next());
    sync.wanted = ApplyStageKit({}, 0x55, kBlue);
    sync.shown = sync.wanted;
    CHECK_FALSE(sync.Next());
}

TEST_CASE("KitSync sends the fog, the strobe, then red, yellow, green and blue") {
    KitSync sync;
    sync.shown = StageKit{};
    sync.wanted = {.red = 1, .yellow = 2, .green = 3, .blue = 4, .strobe = 2, .fog = true};
    CHECK(Drain(sync) == std::vector<Command>{{0, kFogOn},
                                              {0, 0x04},
                                              {1, kRed},
                                              {2, kYellow},
                                              {3, kGreen},
                                              {4, kBlue}});
}

TEST_CASE("KitSync turns the fog and the strobe off") {
    KitSync sync;
    sync.shown = StageKit{.strobe = 4, .fog = true};
    sync.wanted = {};
    CHECK(Drain(sync) == std::vector<Command>{{0, kFogOff}, {0, kStrobeOff}});
}

TEST_CASE("a backlog of commands collapses to the latest lights") {
    KitSync sync;
    sync.shown = StageKit{};
    for (uint8_t mask : {0x01, 0x02, 0x04, 0x08}) {
        sync.wanted = ApplyStageKit(sync.wanted, mask, kRed);
    }
    CHECK(Drain(sync) == std::vector<Command>{{0x08, kRed}});
}

TEST_CASE("a command undone before it's sent isn't sent") {
    KitSync sync;
    sync.shown = StageKit{};
    sync.wanted = ApplyStageKit(sync.wanted, 0, kFogOn);
    sync.wanted = ApplyStageKit(sync.wanted, 0, kFogOff);
    CHECK_FALSE(sync.Next());
}

TEST_CASE("Sent follows what reached the kit, not what it should show") {
    KitSync sync;
    sync.shown = StageKit{};
    sync.wanted = ApplyStageKit({}, 0xFF, kGreen);
    sync.Sent({0x0F, kGreen});
    REQUIRE(sync.shown);
    CHECK(sync.shown->green == 0x0F);
    CHECK(sync.Next() == Command{0xFF, kGreen});
}

TEST_CASE("Santroller Stage Kits are told by their release number") {
    CHECK(IsSantrollerStageKit(0x1209, 0x2882, 0x0900));
    CHECK(IsSantrollerStageKit(0x1209, 0x2882, 0x0912));
    // a Santroller guitar (Rock Band guitar is 6)
    CHECK_FALSE(IsSantrollerStageKit(0x1209, 0x2882, 0x0600));
    CHECK_FALSE(IsSantrollerStageKit(0x1209, 0x2883, 0x0900));
    CHECK_FALSE(IsSantrollerStageKit(0x045E, 0x2882, 0x0900));
}

TEST_CASE("SDL's GUID of a Stage Kit") {
    // bus 3 (USB), CRC, vendor 0x1209, product 0x2882, version 0x0900, no driver
    CHECK(IsStageKitGuid("03001234091200008228000000090000"));
    // the same device as a Santroller guitar
    CHECK_FALSE(IsStageKitGuid("03001234091200008228000000060000"));
    // an XInput device of subtype 9 (driver signature 'x', then the subtype)
    CHECK(IsStageKitGuid("030000005e0400008e02000000007809"));
    // an XInput guitar
    CHECK_FALSE(IsStageKitGuid("030000005e0400008e02000000007806"));
    CHECK_FALSE(IsStageKitGuid(""));
    CHECK_FALSE(IsStageKitGuid("not a guid at all, thirty-two ch"));
}

TEST_CASE("a Santroller kit's HID report") {
    CHECK(HidReport({0x55, kRed}) == std::array<uint8_t, 4>{0x01, 0x5A, 0x55, kRed});
    CHECK(HidReport(kAllOffCommand) == std::array<uint8_t, 4>{0x01, 0x5A, 0x00, 0xFF});
}

TEST_CASE("an Xbox 360 kit's rumble, as the game sends it") {
    CHECK(XInputRumble({0x55, kRed}) == Rumble{0x55FF, 0x80FF});
    CHECK(XInputRumble({0x00, kFogOn}) == Rumble{0x00FF, 0x01FF});
}

TEST_CASE("the colour check lights each colour, then turns everything off") {
    const auto steps = ColourCheck();
    REQUIRE(steps.size() == 5);
    CHECK(steps[0].command == Command{kPatternAll, kRed});
    CHECK(steps[1].command == Command{kPatternAll, kGreen});
    CHECK(steps[2].command == Command{kPatternAll, kBlue});
    CHECK(steps[3].command == Command{kPatternAll, kYellow});
    CHECK(steps.back().command == kAllOffCommand);
    for (size_t i = 0; i + 1 < steps.size(); i++) CHECK(steps[i].delay_ms > 0);
}

TEST_CASE("the chase alternates odd and even LEDs, then turns everything off") {
    const auto steps = Chase();
    REQUIRE(steps.size() == 7);
    CHECK(steps[0].command == Command{kPatternOdds, kRed});
    CHECK(steps[1].command == Command{kPatternEvens, kRed});
    CHECK(steps[5].command == Command{kPatternEvens, kBlue});
    CHECK(steps.back().command == kAllOffCommand);
}
