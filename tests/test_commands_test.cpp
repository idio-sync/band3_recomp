// Checks the test harness's commands (src/Test/test_commands.cpp) against a
// stand-in for the game whose clock only moves when the commands sleep.

#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <optional>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include "src/Test/game_state.h"
#include "src/Test/test_commands.h"

using namespace band3::test;
using namespace std::chrono_literals;
using band3::input::InstrumentInputs;
using band3::input::InstrumentKind;
namespace input = band3::input;

namespace {

struct PulseRecord {
    InstrumentInputs pressed;
    std::chrono::milliseconds length;
};

struct FakePlayer {
    std::optional<InstrumentKind> kind;
    InstrumentInputs held;
    std::vector<PulseRecord> pulses;
    int plugs = 0;
};

class FakeGame final : public TestTarget {
public:
    // player 1 starts plugged in as a guitar, as in the game
    std::array<FakePlayer, 4> players{FakePlayer{InstrumentKind::kGuitar, {}, {}, 0}};
    FakePlayer& player(int n) { return players[n - 1]; }
    // player 1's, which most tests drive
    std::optional<InstrumentKind>& kind = players[0].kind;
    InstrumentInputs& held = players[0].held;
    std::vector<PulseRecord>& pulses = players[0].pulses;
    int& kind_changes = players[0].plugs;
    GameStateSnapshot state;
    // run on every sleep, to change the state as time passes
    std::function<void(FakeGame&)> on_sleep;
    Clock::time_point now{};
    std::chrono::milliseconds slept{0};
    std::vector<std::pair<std::string, std::string>> settings_set;
    std::string screenshot_name;
    bool quit = false;
    bool cancelled = false;
    input::Gamepad360 pad;
    uint32_t pad_packet = 0;
    bool pad_connected = true;
    int pad_player = 0;

    std::optional<InstrumentKind> Kind(int p) override { return player(p).kind; }
    void Plug(int p, InstrumentKind k) override {
        player(p).kind = k;
        player(p).plugs++;
    }
    void Unplug(int p) override { player(p).kind.reset(); }
    InstrumentInputs Held(int p) override { return player(p).held; }
    void SetHeld(int p, const InstrumentInputs& in) override { player(p).held = in; }
    void Pulse(int p, std::function<void(InstrumentInputs&)> change,
               std::chrono::milliseconds length) override {
        InstrumentInputs pressed;
        change(pressed);
        player(p).pulses.push_back({pressed, length});
    }
    GameStateSnapshot State() override { return state; }
    std::string Screenshot(const std::string& name, ScreenshotInfo& out) override {
        screenshot_name = name;
        out.path = "screenshots/" + (name.empty() ? std::string("auto") : name) + ".png";
        out.width = 1280;
        out.height = 720;
        return {};
    }
    std::string SetSetting(std::string_view name, std::string_view value) override {
        if (name == "secret") return "secret isn't a Band3 setting";
        settings_set.emplace_back(name, value);
        return {};
    }
    void Quit() override { quit = true; }
    bool Cancelled() override { return cancelled; }
    bool ReadPad(int player, input::Gamepad360& out, uint32_t& packet) override {
        pad_player = player;
        out = pad;
        packet = pad_packet;
        return pad_connected;
    }
    Clock::time_point Now() override { return now; }
    void Sleep(std::chrono::milliseconds length) override {
        now += length;
        slept += length;
        if (on_sleep) on_sleep(*this);
    }
};

bool Ok(const std::string& reply) { return reply.starts_with("{\"ok\":true"); }
bool Has(const std::string& reply, std::string_view text) {
    return reply.find(text) != std::string::npos;
}

}

TEST_CASE("an unknown or empty command is an error reply that carries the state") {
    FakeGame game;
    game.state.screen = "main_hub_screen";
    const std::string reply = RunCommand("dance", game);
    CHECK(reply.starts_with("{\"ok\":false"));
    CHECK(Has(reply, "dance"));
    CHECK(Has(reply, "\"state\":{"));
    CHECK(Has(reply, "main_hub_screen"));
    CHECK_FALSE(Ok(RunCommand("", game)));
}

TEST_CASE("replies are one line of JSON") {
    FakeGame game;
    game.state.screen = "a\"screen\nwith\\odd chars";
    const std::string reply = RunCommand("state", game);
    CHECK(Ok(reply));
    CHECK(reply.find('\n') == std::string::npos);
    CHECK(Has(reply, R"("screen":"a\"screen\nwith\\odd chars")"));
}

TEST_CASE("state reports the game state and the instrument") {
    FakeGame game;
    game.kind = InstrumentKind::kDrums;
    game.state.in_game = true;
    game.state.song_shortname = "ruby";
    game.state.song_name = "Ruby";
    game.state.frame = 42;
    game.state.band[0] = {true, 3, 2};
    const std::string reply = RunCommand("state", game);
    CHECK(Has(reply, "\"in_game\":true"));
    CHECK(Has(reply, "\"shortname\":\"ruby\""));
    CHECK(Has(reply, "\"name\":\"Ruby\""));
    CHECK(Has(reply, "\"frame\":42"));
    CHECK(Has(reply, "\"instruments\":[\"drums\",null,null,null]"));
    CHECK(Has(reply, "{\"exists\":true,\"difficulty\":3,\"track\":2}"));
}

TEST_CASE("press pulses every joined input for the time asked, then waits for the release") {
    FakeGame game;
    CHECK(Ok(RunCommand("press green+red+strum_down 200", game)));
    REQUIRE(game.pulses.size() == 1);
    const auto& pulse = game.pulses[0];
    CHECK(pulse.length == 200ms);
    CHECK(pulse.pressed.guitar.frets[input::kGreen]);
    CHECK(pulse.pressed.guitar.frets[input::kRed]);
    CHECK(pulse.pressed.guitar.strum_down);
    // so the next press is a new one
    CHECK(game.slept > 200ms);
}

TEST_CASE("press defaults to 100 ms") {
    FakeGame game;
    CHECK(Ok(RunCommand("press a", game)));
    REQUIRE(game.pulses.size() == 1);
    CHECK(game.pulses[0].length == 100ms);
}

TEST_CASE("a press with any bad input presses nothing") {
    FakeGame game;
    const std::string reply = RunCommand("press green+red_pad", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "red_pad"));
    CHECK(game.pulses.empty());
    CHECK_FALSE(Ok(RunCommand("press green 0", game)));
    CHECK_FALSE(Ok(RunCommand("press green soon", game)));
    CHECK_FALSE(Ok(RunCommand("press", game)));
}

TEST_CASE("hold keeps inputs down until released") {
    FakeGame game;
    CHECK(Ok(RunCommand("hold orange+solo", game)));
    CHECK(game.held.guitar.frets[input::kOrange]);
    CHECK(game.held.guitar.solo);
    CHECK(Ok(RunCommand("release solo", game)));
    CHECK(game.held.guitar.frets[input::kOrange]);
    CHECK_FALSE(game.held.guitar.solo);
    CHECK(Ok(RunCommand("axis whammy 0.75", game)));
    CHECK(Ok(RunCommand("release all", game)));
    CHECK_FALSE(game.held.guitar.frets[input::kOrange]);
    CHECK(game.held.guitar.whammy == doctest::Approx(0.0f));
}

TEST_CASE("axis sets a held analog value") {
    FakeGame game;
    CHECK(Ok(RunCommand("axis whammy 0.75", game)));
    CHECK(game.held.guitar.whammy == doctest::Approx(0.75f));
    CHECK_FALSE(Ok(RunCommand("axis whammy loud", game)));
    CHECK_FALSE(Ok(RunCommand("axis whammy 2", game)));
}

TEST_CASE("hit sounds a pad, key or string at a velocity") {
    FakeGame game;
    game.kind = InstrumentKind::kDrums;
    CHECK(Ok(RunCommand("hit yellow_cym 64", game)));
    REQUIRE(game.pulses.size() == 1);
    CHECK(game.pulses[0].pressed.drums.cymbals[input::kYellowCymbal] == 64);

    CHECK(Ok(RunCommand("hit red_pad", game)));
    CHECK(game.pulses.back().pressed.drums.pads[input::kRedPad] == 100);

    game.kind = InstrumentKind::kProGuitarSquier;
    CHECK(Ok(RunCommand("hit g_str 90 7", game)));
    CHECK(game.pulses.back().pressed.pro_guitar.frets[input::kStringG] == 7);
    CHECK(game.pulses.back().pressed.pro_guitar.velocities[input::kStringG] == 90);

    CHECK_FALSE(Ok(RunCommand("hit g_str 128", game)));
    CHECK_FALSE(Ok(RunCommand("hit g_str 0", game)));
}

TEST_CASE("instrument switches the virtual instrument and gives it time to replug") {
    FakeGame game;
    CHECK(Ok(RunCommand("instrument mustang", game)));
    CHECK(game.kind == InstrumentKind::kProGuitarMustang);
    CHECK(game.slept >= 1s);

    // already that instrument: nothing to replug
    game.slept = 0ms;
    CHECK(Ok(RunCommand("instrument mustang", game)));
    CHECK(game.kind_changes == 1);
    CHECK(game.slept == 0ms);

    CHECK_FALSE(Ok(RunCommand("instrument banjo", game)));
}

TEST_CASE("wait returns as soon as the condition holds") {
    FakeGame game;
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 500ms) g.state.screen = "main_hub_screen";
    };
    const std::string reply = RunCommand("wait screen=main_hub_screen", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "main_hub_screen"));
    CHECK(game.slept >= 500ms);
    CHECK(game.slept < 600ms);
}

TEST_CASE("wait fails at its timeout, with the state it last saw") {
    FakeGame game;
    game.state.screen = "loading_screen";
    const std::string reply = RunCommand("wait in_game timeout=2s", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "timed out"));
    CHECK(Has(reply, "loading_screen"));
    CHECK(game.slept >= 2s);
    CHECK(game.slept < 2100ms);
}

TEST_CASE("wait's default timeout is 30 s, expect's is 5 s") {
    FakeGame game;
    CHECK_FALSE(Ok(RunCommand("wait in_game", game)));
    CHECK(game.slept >= 30s);
    CHECK(game.slept < 31s);

    game.slept = 0ms;
    CHECK_FALSE(Ok(RunCommand("expect in_game", game)));
    CHECK(game.slept >= 5s);
    CHECK(game.slept < 6s);

    game.slept = 0ms;
    CHECK_FALSE(Ok(RunCommand("expect in_game timeout=250ms", game)));
    CHECK(game.slept >= 250ms);
    CHECK(game.slept < 350ms);
}

TEST_CASE("wait gives up when the harness shuts down") {
    FakeGame game;
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 300ms) g.cancelled = true;
    };
    const std::string reply = RunCommand("wait in_game", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "shutting down"));
    CHECK(game.slept < 400ms);
}

TEST_CASE("wait conditions") {
    GameStateSnapshot s;
    s.screen = "song_select_screen";
    s.in_game = false;
    s.song_shortname = "ruby";
    s.frame = 110;

    auto holds = [&](std::string_view text, uint64_t start_frame = 100) {
        auto parsed = ParseCondition(text);
        REQUIRE(std::holds_alternative<Condition>(parsed));
        return ConditionHolds(std::get<Condition>(parsed), s, start_frame);
    };
    CHECK(holds("screen=song_select_screen"));
    CHECK_FALSE(holds("screen=song_select"));
    CHECK(holds("screen~select"));
    CHECK_FALSE(holds("screen~results"));
    CHECK(holds("menus"));
    CHECK_FALSE(holds("in_game"));
    CHECK(holds("song=ruby"));
    CHECK_FALSE(holds("song=rubyx"));
    CHECK(holds("frames=10"));
    CHECK_FALSE(holds("frames=11"));

    CHECK(std::holds_alternative<std::string>(ParseCondition("screen")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("frames=many")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("loud")));
}

TEST_CASE("a bad wait condition or timeout is an error, not a wait") {
    FakeGame game;
    CHECK_FALSE(Ok(RunCommand("wait screen", game)));
    CHECK_FALSE(Ok(RunCommand("wait in_game timeout=soon", game)));
    CHECK_FALSE(Ok(RunCommand("wait", game)));
    CHECK(game.slept == 0ms);
}

TEST_CASE("screenshot names are plain file names") {
    FakeGame game;
    const std::string reply = RunCommand("screenshot main-menu_1", game);
    CHECK(Ok(reply));
    CHECK(game.screenshot_name == "main-menu_1");
    CHECK(Has(reply, "\"width\":1280"));
    CHECK(Has(reply, "\"path\":\"screenshots/main-menu_1.png\""));

    CHECK(Ok(RunCommand("screenshot", game)));
    CHECK(game.screenshot_name == "");

    game.screenshot_name = "unchanged";
    CHECK_FALSE(Ok(RunCommand("screenshot ../../evil", game)));
    CHECK_FALSE(Ok(RunCommand("screenshot C:evil", game)));
    CHECK(game.screenshot_name == "unchanged");
}

TEST_CASE("set passes the setting on, keeping spaces in the value") {
    FakeGame game;
    CHECK(Ok(RunCommand("set forced_venue arena_04", game)));
    CHECK(Ok(RunCommand("set username Some Name", game)));
    REQUIRE(game.settings_set.size() == 2);
    CHECK(game.settings_set[0] == std::pair<std::string, std::string>{"forced_venue", "arena_04"});
    CHECK(game.settings_set[1].second == "Some Name");

    const std::string reply = RunCommand("set secret 1", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "isn't a Band3 setting"));
    CHECK_FALSE(Ok(RunCommand("set autoplay", game)));
}

TEST_CASE("pad reports what the game reads from a player") {
    FakeGame game;
    game.pad.buttons = input::xbox::kButtonA | input::xbox::kDpadUp;
    game.pad.thumb_rx = -32768;
    game.pad_packet = 77;
    const std::string reply = RunCommand("pad", game);
    CHECK(Ok(reply));
    CHECK(game.pad_player == 1);
    CHECK(Has(reply, "\"connected\":true"));
    CHECK(Has(reply, "\"buttons\":[\"up\",\"a\"]"));
    CHECK(Has(reply, "\"rx\":-32768"));
    CHECK(Has(reply, "\"packet\":77"));

    CHECK(Ok(RunCommand("pad 3", game)));
    CHECK(game.pad_player == 3);
    CHECK_FALSE(Ok(RunCommand("pad 5", game)));
    CHECK_FALSE(Ok(RunCommand("pad one", game)));

    game.pad_connected = false;
    CHECK(Has(RunCommand("pad 2", game), "\"connected\":false"));
}

TEST_CASE("a pN prefix sends a controller command to that player") {
    FakeGame game;
    CHECK(Ok(RunCommand("p2 instrument drums", game)));
    CHECK(game.player(2).kind == InstrumentKind::kDrums);
    CHECK(game.player(1).kind == InstrumentKind::kGuitar);

    CHECK(Ok(RunCommand("p2 hit red_pad 90", game)));
    REQUIRE(game.player(2).pulses.size() == 1);
    CHECK(game.player(2).pulses[0].pressed.drums.pads[input::kRedPad] == 90);
    CHECK(game.player(1).pulses.empty());

    CHECK(Ok(RunCommand("p1 press green", game)));
    CHECK(game.player(1).pulses.size() == 1);
    CHECK(game.player(2).pulses.size() == 1);
}

TEST_CASE("hold and release act on their own player only") {
    FakeGame game;
    CHECK(Ok(RunCommand("p3 instrument guitar", game)));
    CHECK(Ok(RunCommand("hold green", game)));
    CHECK(Ok(RunCommand("p3 hold green+orange", game)));
    CHECK(Ok(RunCommand("p3 release all", game)));
    CHECK(game.player(1).held.guitar.frets[input::kGreen]);
    CHECK_FALSE(game.player(3).held.guitar.frets[input::kGreen]);
    CHECK(Ok(RunCommand("p3 axis whammy 1", game)));
    CHECK(game.player(3).held.guitar.whammy == doctest::Approx(1.0f));
    CHECK(game.player(1).held.guitar.whammy == doctest::Approx(0.0f));
}

TEST_CASE("driving a player with nothing plugged in says how to plug one in") {
    FakeGame game;
    for (const char* line : {"p3 press green", "p3 hold green", "p3 release all",
                             "p3 hit red_pad", "p3 axis whammy 0.5"}) {
        const std::string reply = RunCommand(line, game);
        CHECK_FALSE(Ok(reply));
        CHECK(Has(reply, "player 3 has no virtual instrument"));
        CHECK(Has(reply, "p3 instrument"));
    }
}

TEST_CASE("an input the player's instrument lacks names the player") {
    FakeGame game;
    CHECK(Ok(RunCommand("p2 instrument drums", game)));
    const std::string reply = RunCommand("p2 press green", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "player 2"));
    CHECK(Has(reply, "green"));
}

TEST_CASE("unplug takes a player's instrument out") {
    FakeGame game;
    CHECK(Ok(RunCommand("p2 instrument keys", game)));
    CHECK(Ok(RunCommand("p2 hold key3", game)));
    CHECK(Ok(RunCommand("p2 unplug", game)));
    CHECK_FALSE(game.player(2).kind.has_value());
    CHECK(game.player(2).held.keys.keys[3] == 0);
    // already empty: nothing to do
    CHECK(Ok(RunCommand("p2 unplug", game)));
    CHECK(Ok(RunCommand("unplug", game)));
    CHECK_FALSE(game.player(1).kind.has_value());
}

TEST_CASE("state lists every player's instrument") {
    FakeGame game;
    CHECK(Ok(RunCommand("p3 instrument squier", game)));
    CHECK(Has(RunCommand("state", game), "\"instruments\":[\"guitar\",null,\"squier\",null]"));
}

TEST_CASE("a bad or misplaced player prefix is an error") {
    FakeGame game;
    for (const char* line : {"p0 press green", "p5 press green", "px press green", "p2",
                             "p2 state", "p2 wait in_game", "p2 expect menus",
                             "p2 screenshot", "p2 set autoplay true", "p2 quit"}) {
        CHECK_FALSE(Ok(RunCommand(line, game)));
    }
    CHECK_FALSE(game.quit);
    CHECK(game.slept == 0ms);
}

TEST_CASE("pad takes a player as an argument or a prefix, not both") {
    FakeGame game;
    CHECK(Ok(RunCommand("p3 pad", game)));
    CHECK(game.pad_player == 3);
    CHECK_FALSE(Ok(RunCommand("p3 pad 2", game)));
}

TEST_CASE("releasing everything lets go on every player, instruments stay plugged in") {
    FakeGame game;
    CHECK(Ok(RunCommand("p2 instrument guitar", game)));
    CHECK(Ok(RunCommand("hold green", game)));
    CHECK(Ok(RunCommand("p2 hold red", game)));
    ReleaseAllPlayers(game);
    CHECK_FALSE(game.player(1).held.guitar.frets[input::kGreen]);
    CHECK_FALSE(game.player(2).held.guitar.frets[input::kRed]);
    CHECK(game.player(2).kind == InstrumentKind::kGuitar);
}

TEST_CASE("quit asks the game to close") {
    FakeGame game;
    CHECK(Ok(RunCommand("quit", game)));
    CHECK(game.quit);
}

TEST_CASE("the game state counts frames and keeps the latest of everything") {
    GameState state;
    state.SetScreen("main_hub_screen");
    state.SetInGame(true);
    state.SetSong("Ruby", "Kaiser Chiefs", "ruby");
    state.SetVenue("arena_04");
    state.CountFrame();
    state.CountFrame();
    const GameStateSnapshot s = state.Snapshot();
    CHECK(s.screen == "main_hub_screen");
    CHECK(s.in_game);
    CHECK(s.song_artist == "Kaiser Chiefs");
    CHECK(s.venue == "arena_04");
    CHECK(s.frame == 2);

    // leaving the song keeps what it was, for the results screen
    state.SetInGame(false);
    CHECK(state.Snapshot().song_shortname == "ruby");
}
