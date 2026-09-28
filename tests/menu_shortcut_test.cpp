// Checks the controller menu shortcut (src/Input/menu_shortcut.cpp) and the
// Steam Deck detection (src/steam_deck.h).

#include <doctest/doctest.h>
#include <chrono>
#include <cstdint>
#include "src/Input/instruments.h"
#include "src/Input/menu_shortcut.h"
#include "src/steam_deck.h"

using namespace band3::input;
using namespace std::chrono_literals;

namespace {

const MenuShortcut::Clock::time_point t0{};
constexpr uint16_t kSticks = xbox::kLeftThumb | xbox::kRightThumb;
constexpr uint16_t kSticksAndBumper = kSticks | xbox::kLeftShoulder;

}

TEST_CASE("holding both stick clicks for a second opens the settings menu, once") {
    MenuShortcut shortcut;
    CHECK(shortcut.Update(kSticks, t0) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 999ms) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 1s) == MenuShortcutAction::kSettings);
    CHECK(shortcut.Update(kSticks, t0 + 5s) == MenuShortcutAction::kNone);

    // let go, and it fires again
    CHECK(shortcut.Update(0, t0 + 6s) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 7s) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 8s) == MenuShortcutAction::kSettings);
}

TEST_CASE("the left bumper with the stick clicks opens the Instrument Lab") {
    MenuShortcut shortcut;
    CHECK(shortcut.Update(kSticksAndBumper, t0) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 1s) == MenuShortcutAction::kInstrumentLab);
}

TEST_CASE("changing the chord restarts the hold") {
    MenuShortcut shortcut;
    CHECK(shortcut.Update(kSticks, t0) == MenuShortcutAction::kNone);
    // the bumper joins late: the Instrument Lab's second starts now
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 800ms) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 1500ms) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 1800ms) == MenuShortcutAction::kInstrumentLab);
}

TEST_CASE("a fired chord waits for all its buttons to be let go") {
    MenuShortcut shortcut;
    shortcut.Update(kSticks, t0);
    CHECK(shortcut.Update(kSticks, t0 + 1s) == MenuShortcutAction::kSettings);
    // adding the bumper after the settings menu opened doesn't also open the lab
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 2s) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticksAndBumper, t0 + 4s) == MenuShortcutAction::kNone);
    // nor does letting go of one stick and clicking it again
    CHECK(shortcut.Update(xbox::kRightThumb, t0 + 5s) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 5500ms) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(kSticks, t0 + 7s) == MenuShortcutAction::kNone);
}

TEST_CASE("other buttons don't matter, one stick click isn't a chord") {
    MenuShortcut shortcut;
    const uint16_t with_frets = kSticks | xbox::kButtonA | xbox::kStart;
    CHECK(shortcut.Update(with_frets, t0) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(with_frets, t0 + 1s) == MenuShortcutAction::kSettings);

    MenuShortcut one_stick;
    CHECK(one_stick.Update(xbox::kLeftThumb | xbox::kLeftShoulder, t0) == MenuShortcutAction::kNone);
    CHECK(one_stick.Update(xbox::kLeftThumb | xbox::kLeftShoulder, t0 + 10s) ==
          MenuShortcutAction::kNone);
}

TEST_CASE("Steam Deck detection") {
    using band3::steam_deck::LooksLikeSteamDeck;
    CHECK(LooksLikeSteamDeck("1", "", ""));
    CHECK(LooksLikeSteamDeck("", "Valve", "Jupiter"));
    CHECK(LooksLikeSteamDeck("", "Valve", "Galileo"));
    CHECK_FALSE(LooksLikeSteamDeck("", "", ""));
    CHECK_FALSE(LooksLikeSteamDeck("0", "Valve", "Index"));
    CHECK_FALSE(LooksLikeSteamDeck("", "LENOVO", "Jupiter"));
}
