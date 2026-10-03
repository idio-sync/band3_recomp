// Checks the controller menu shortcut (src/Input/menu_shortcut.cpp), the
// controllers it watches (ChordPads), and the Steam Deck detection and presets
// (src/steam_deck.h).

#include <doctest/doctest.h>
#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <vector>
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

TEST_CASE("the shortcut sees what the game read from each player's controller") {
    ChordPads pads;
    // nothing read yet
    CHECK(pads.Held(t0) == 0);
    pads.OnCapabilities(0, kSubtypeGamepad);
    pads.OnState(0, kSticks, t0);
    CHECK(pads.Held(t0 + 10ms) == kSticks);
    // another player's buttons add to them
    pads.OnCapabilities(2, kSubtypeGamepad);
    pads.OnState(2, xbox::kLeftShoulder | xbox::kButtonA, t0);
    CHECK(pads.Held(t0 + 10ms) == (kSticksAndBumper | xbox::kButtonA));
    // let go, or unplugged
    pads.OnState(0, 0, t0 + 20ms);
    pads.OnState(2, std::nullopt, t0 + 20ms);
    CHECK(pads.Held(t0 + 30ms) == 0);
    // players past the fourth aren't read
    pads.OnCapabilities(4, kSubtypeGamepad);
    pads.OnState(4, kSticks, t0 + 30ms);
    CHECK(pads.Held(t0 + 40ms) == 0);
}

TEST_CASE("instruments don't count, nor a controller of no known type") {
    ChordPads pads;
    // a kit's pad flag and second kick are the stick clicks, its kick the bumper
    pads.OnCapabilities(0, kSubtypeDrums);
    pads.OnState(0, kSticksAndBumper, t0);
    // a guitar's solo frets are the left stick click
    pads.OnCapabilities(1, kSubtypeGuitar);
    pads.OnState(1, xbox::kLeftThumb, t0);
    // read before its type was
    pads.OnState(2, kSticks, t0);
    CHECK(pads.Held(t0) == 0);

    // the same player as a controller: its type is read again when it connects
    pads.OnCapabilities(0, kSubtypeGamepad);
    CHECK(pads.Held(t0) == kSticksAndBumper);
}

TEST_CASE("a player the game stopped reading holds nothing") {
    ChordPads pads;
    pads.OnCapabilities(1, kSubtypeGamepad);
    pads.OnState(1, kSticks, t0);
    CHECK(pads.Held(t0 + ChordPads::kStale) == kSticks);
    CHECK(pads.Held(t0 + ChordPads::kStale + 1ms) == 0);
    pads.OnState(1, kSticks, t0 + 2s);
    CHECK(pads.Held(t0 + 2s) == kSticks);

    // so a chord held as the reads stop doesn't fire
    MenuShortcut shortcut;
    CHECK(shortcut.Update(pads.Held(t0 + 2s), t0 + 2s) == MenuShortcutAction::kNone);
    CHECK(shortcut.Update(pads.Held(t0 + 3s), t0 + 3s) == MenuShortcutAction::kNone);
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

namespace {

// a cvar at startup: its value, and whether anything but the default set it
struct StartupCvar {
    std::string value;
    bool set = false;
};

}

TEST_CASE("the Steam Deck presets win over band3_config.ini, even at the cvar's default") {
    using namespace band3::steam_deck;
    // the registry's defaults: fullscreen = true is the preset's value already
    std::map<std::string, StartupCvar, std::less<>> cvars = {
        {"fullscreen", {"true"}},
        {"present_letterbox", {"false"}},
        {"rnd_sync", {"-1"}},
        {"debug_overlay", {"true"}},
    };
    // band3.toml set this one
    cvars["rnd_sync"] = {"0", true};
    std::vector<std::string> refused_names;
    const PresetCvars access{
        .set_elsewhere = [&](std::string_view cvar) { return cvars[std::string(cvar)].set; },
        .set =
            [&](std::string_view cvar, std::string_view value) {
                if (cvar == "debug_overlay") return false;
                cvars[std::string(cvar)] = {std::string(value), true};
                return true;
            },
    };
    const int applied = ApplyPresets(
        access, [&](const DeckDefault& d) { refused_names.emplace_back(d.cvar); });

    CHECK(applied == 2);
    CHECK(refused_names == std::vector<std::string>{"debug_overlay"});
    // set although it had the value, so it counts as set
    CHECK(cvars["fullscreen"].value == "true");
    CHECK(cvars["fullscreen"].set);
    CHECK(cvars["present_letterbox"].value == "true");
    // band3.toml's stays
    CHECK(cvars["rnd_sync"].value == "0");

    // then ApplyLegacyIni fills in only what nothing set: a desktop ini's
    // [window] fullscreen = false doesn't undo the preset
    const std::map<std::string, std::string> ini = {{"fullscreen", "false"},
                                                    {"present_letterbox", "false"}};
    for (const auto& [cvar, value] : ini) {
        if (!access.set_elsewhere(cvar)) access.set(cvar, value);
    }
    CHECK(cvars["fullscreen"].value == "true");
    CHECK(cvars["present_letterbox"].value == "true");
}

TEST_CASE("the launcher reads the same presets ApplyDefaults sets") {
    using band3::steam_deck::Preset;
    CHECK(Preset("fullscreen") == "true");
    CHECK(Preset("rnd_sync") == "1");
    CHECK_FALSE(Preset("lang"));
}
