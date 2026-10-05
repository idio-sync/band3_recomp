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
    std::vector<std::string> binds_pressed;
    GameFolders folders;
    struct InviteRecord {
        std::string host;
        uint16_t port;
        bool force_flag;
    };
    std::vector<InviteRecord> invites;
    std::string screenshot_name;
    ScreenshotSource screenshot_source = ScreenshotSource::kWindow;
    // renderer = native: the window's picture is the native renderer's, and
    // its size the window's
    bool native = false;
    std::string capture_name;
    bool gpu_works = true;
    // what the capture is: a composed post frame, or the frame a capture
    // took when none came (held_fallback)
    bool capture_composed = true;
    int64_t capture_proc_cmds = 2;
    bool capture_fell_back = false;
    std::string capture_emulated = "full";
    uint64_t capture_passes_dropped = 0;
    NativeViewStats view;
    PresentStats present;
    int present_resets = 0;
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
    std::string Screenshot(const std::string& name, ScreenshotSource source,
                           ScreenshotInfo& out) override {
        screenshot_name = name;
        screenshot_source = source;
        const bool drawn_native = source == ScreenshotSource::kNative ||
                                  (source == ScreenshotSource::kWindow && native);
        out.path = "screenshots/" + (name.empty() ? std::string("auto") : name) + ".png";
        out.width = drawn_native ? 1600 : 1280;
        out.height = drawn_native ? 900 : 720;
        out.renderer = drawn_native ? "native" : "emulated";
        return {};
    }
    std::string Capture(const std::string& name, CaptureInfo& out) override {
        capture_name = name;
        const std::string file = "screenshots/" + (name.empty() ? std::string("auto") : name);
        out.screenshot = {file + ".png", 1280, 720};
        out.capture_path = file + ".cap";
        out.frame = 42;
        out.draws = 345;
        out.skipped_shadow = 12;
        out.skipped_pass = 3;
        out.passes = 30;
        out.passes_carried = 9;
        out.rt_sampled = 14;
        out.rt_missing = 1;
        out.rt_filtered = 3;
        out.rt_fallback = "none";
        out.proc_cmds = capture_proc_cmds;
        out.composed = capture_composed;
        out.held_fallback = capture_fell_back;
        out.emulated = capture_emulated;
        out.emulated_passes_dropped = capture_passes_dropped;
        out.game_frame = 2401;
        out.world_frame = 2400;
        if (gpu_works) {
            out.gpu_path = file + ".gpu.png";
            out.gpu_ms = 4.24;
            out.gpu_wait_ms = 1.04;
        } else {
            out.gpu_error = "no GPU device";
        }
        return {};
    }
    std::string SetSetting(std::string_view name, std::string_view value) override {
        if (name == "secret") return "secret isn't a Band3 setting";
        settings_set.emplace_back(name, value);
        return {};
    }
    std::optional<SettingValue> GetSetting(std::string_view name) override {
        if (name == "lang") return SettingValue{"fre", "config"};
        if (name == "username") return SettingValue{"Some \"Name\"", "default"};
        return std::nullopt;
    }
    GameFolders Folders() override { return folders; }
    std::string PressBind(std::string_view bind) override {
        if (bind == "bind_nothing") return "no key bind bind_nothing";
        binds_pressed.emplace_back(bind);
        return {};
    }
    std::string LivelessInvite(const std::string& host, uint16_t port, bool force_flag) override {
        if (host == "offline") return "liveless is off";
        invites.push_back({host, port, force_flag});
        return {};
    }
    band3::rooms::Status rooms;
    std::vector<std::string> rooms_joins;
    int rooms_connects = 0;
    band3::rooms::Status RoomsStatus() override { return rooms; }
    std::string RoomsJoin(const std::string& code) override {
        if (rooms.state != band3::rooms::State::kLoggedIn) return "not logged in to the Rooms server";
        if (!rooms.game_socket_seen) return "the game isn't online yet: Play on Xbox Live first";
        rooms_joins.push_back(code);
        return {};
    }
    std::string RoomsConnect() override {
        if (rooms.state == band3::rooms::State::kOff) return "Liveless Rooms isn't running";
        rooms_connects++;
        return {};
    }
    band3::port_mapping::Status port_mapping;
    band3::port_mapping::Status PortMappingStatus() override { return port_mapping; }
    std::string NativeViewOn(uint32_t width, uint32_t height, bool sized, bool post) override {
        if (width > 4000) return "no GPU target that big";
        if (sized && native) return "renderer is native: its size follows the window's";
        view = NativeViewStats{};
        view.on = true;
        view.backend = "gpu";
        view.width = width;
        view.height = height;
        view.post = post;
        return {};
    }
    void NativeViewOff() override { view = NativeViewStats{}; }
    NativeViewStats NativeView() override { return view; }
    PresentStats Present(bool reset) override {
        const PresentStats out = present;
        if (reset) {
            present = PresentStats{};
            present_resets++;
        }
        return out;
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
    CHECK(Has(reply, "\"score\":0"));
    // no USB mics recording, so no mic slots
    CHECK_FALSE(Has(reply, "\"mics\""));
}

TEST_CASE("state reports the score and the USB mic slots") {
    FakeGame game;
    game.state.score = 3000;
    game.state.mics = {{"test tone", true, 64000}, {}, {}, {}};
    const std::string reply = RunCommand("state", game);
    CHECK(Has(reply, "\"score\":3000"));
    CHECK(Has(reply, "\"mics\":[{\"device\":\"test tone\",\"connected\":true,\"fed\":64000},"
                     "{\"device\":\"\",\"connected\":false,\"fed\":0},"));
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

TEST_CASE("press until: one press when the screen changes") {
    FakeGame game;
    game.state.screen = "dx_welcome_screen";
    game.on_sleep = [](FakeGame& g) {
        if (!g.pulses.empty() && g.slept > 300ms) g.state.screen = "hint_rb3_welcome_screen";
    };
    const std::string reply =
        RunCommand("press green until screen=hint_rb3_welcome_screen", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "hint_rb3_welcome_screen"));
    CHECK(game.pulses.size() == 1);
}

TEST_CASE("press until: pressed again while the screen stays, every 2 s by default") {
    FakeGame game;
    game.state.screen = "dx_welcome_screen";
    // the first two presses are dropped, the third takes
    game.on_sleep = [](FakeGame& g) {
        if (g.pulses.size() >= 3) g.state.screen = "hint_rb3_welcome_screen";
    };
    CHECK(Ok(RunCommand("press green until screen=hint_rb3_welcome_screen", game)));
    CHECK(game.pulses.size() == 3);
    // two waits of 2 s before the third press
    CHECK(game.slept >= 4000ms);
    CHECK(game.slept < 5000ms);
}

TEST_CASE("press until: `every` and `timeout` are the press's, and the timeout counts presses") {
    FakeGame game;
    game.state.screen = "main_hub_screen";
    const std::string reply = RunCommand(
        "press green until screen=song_select_screen every=500ms timeout=2s", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "timed out after 2000 ms and"));
    CHECK(Has(reply, "presses waiting for screen=song_select_screen"));
    // a press every 500 ms (and its 100 ms and the release gap) over 2 s
    CHECK(game.pulses.size() >= 3);
    CHECK(game.pulses.size() <= 4);
}

TEST_CASE("press until: a screen that changes slowly isn't pressed into twice") {
    FakeGame game;
    game.state.screen = "a";
    // the press takes, but the screen changes 1.5 s later
    game.on_sleep = [](FakeGame& g) {
        if (!g.pulses.empty() && g.slept > 1500ms) g.state.screen = "b";
    };
    CHECK(Ok(RunCommand("press green until screen=b", game)));
    CHECK(game.pulses.size() == 1);
}

TEST_CASE("press until: once the screen has changed at all, it isn't pressed again") {
    FakeGame game;
    game.state.screen = "main_hub_screen";
    // the press takes: a loading screen for 5 s, then the one waited for
    game.on_sleep = [](FakeGame& g) {
        if (g.pulses.empty()) return;
        g.state.screen = g.slept > 5000ms ? "song_select_screen" : "loading_screen";
    };
    CHECK(Ok(RunCommand("press green until screen=song_select_screen", game)));
    CHECK(game.pulses.size() == 1);
}

TEST_CASE("press until: bad conditions and options press nothing") {
    FakeGame game;
    CHECK_FALSE(Ok(RunCommand("press green until", game)));
    CHECK_FALSE(Ok(RunCommand("press green until nowhere", game)));
    CHECK_FALSE(Ok(RunCommand("press green until in_game every=soon", game)));
    CHECK_FALSE(Ok(RunCommand("press green until in_game every=0ms", game)));
    CHECK_FALSE(Ok(RunCommand("press green until in_game later=2s", game)));
    CHECK_FALSE(Ok(RunCommand("press green 100 50 until in_game", game)));
    CHECK(game.pulses.empty());
    // a length still goes before `until`
    game.state.in_game = true;
    CHECK(Ok(RunCommand("press green 200 until in_game", game)));
    REQUIRE(game.pulses.size() == 1);
    CHECK(game.pulses[0].length == 200ms);
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

TEST_CASE("sleep waits the wall-clock time asked, whatever the game does") {
    FakeGame game;
    CHECK(Ok(RunCommand("sleep 2s", game)));
    CHECK(game.slept == 2s);

    game.slept = 0ms;
    CHECK(Ok(RunCommand("sleep 250ms", game)));
    CHECK(game.slept == 250ms);

    game.slept = 0ms;
    for (const char* bad : {"sleep", "sleep 2", "sleep two", "sleep 601s", "sleep 1s 2s"}) {
        INFO(bad);
        CHECK_FALSE(Ok(RunCommand(bad, game)));
    }
    CHECK(game.slept == 0ms);

    game.cancelled = true;
    const std::string reply = RunCommand("sleep 1s", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "shutting down"));
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

    s.score = 2500;
    s.mics = {{"Yeti", true, 9000}, {"Blue", false, 0}, {}, {}};
    GameStateSnapshot start = s;
    start.frame = 100;
    start.mics[0].bytes_fed = 4000;

    auto holds = [&](std::string_view text) {
        auto parsed = ParseCondition(text);
        REQUIRE(std::holds_alternative<Condition>(parsed));
        return ConditionHolds(std::get<Condition>(parsed), s, start);
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
    CHECK(holds("score>=2500"));
    CHECK_FALSE(holds("score>=2501"));
    // connected and fed since the wait began
    CHECK(holds("mic=1"));
    // not connected
    CHECK_FALSE(holds("mic=2"));
    start.mics[0].bytes_fed = 9000;
    // connected, but fed nothing since
    CHECK_FALSE(holds("mic=1"));
    s.mics.clear();
    // no USB mics recording
    CHECK_FALSE(holds("mic=1"));

    CHECK(std::holds_alternative<std::string>(ParseCondition("screen")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("frames=many")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("loud")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("score>=lots")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("mic=0")));
    CHECK(std::holds_alternative<std::string>(ParseCondition("mic=5")));
}

TEST_CASE("wait mic= waits for the slot to be fed while it waits") {
    FakeGame game;
    game.state.mics = {{"test tone", true, 1000}, {}, {}, {}};
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 100ms) g.state.mics[0].bytes_fed = 1640;
    };
    CHECK(Ok(RunCommand("wait mic=1", game)));
    CHECK(game.slept >= 100ms);

    // audio fed before the wait isn't enough
    FakeGame idle;
    idle.state.mics = {{"test tone", true, 1000}, {}, {}, {}};
    CHECK_FALSE(Ok(RunCommand("expect mic=1 timeout=1s", idle)));
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

TEST_CASE("screenshot takes the window's picture, or the renderer named") {
    FakeGame game;
    std::string reply = RunCommand("screenshot menu", game);
    CHECK(Ok(reply));
    CHECK(game.screenshot_source == ScreenshotSource::kWindow);
    CHECK(Has(reply, "\"width\":1280,\"height\":720,\"renderer\":\"emulated\""));

    // renderer = native: the window shows the native renderer's, at its size
    game.native = true;
    reply = RunCommand("screenshot", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"width\":1600,\"height\":900,\"renderer\":\"native\""));

    // either one, whatever the window shows, with or without a name
    reply = RunCommand("screenshot emulated song-1", game);
    CHECK(Ok(reply));
    CHECK(game.screenshot_source == ScreenshotSource::kEmulated);
    CHECK(game.screenshot_name == "song-1");
    CHECK(Has(reply, "\"renderer\":\"emulated\""));
    game.native = false;
    reply = RunCommand("screenshot native", game);
    CHECK(Ok(reply));
    CHECK(game.screenshot_source == ScreenshotSource::kNative);
    CHECK(game.screenshot_name == "");
    CHECK(Has(reply, "\"renderer\":\"native\""));

    game.screenshot_name = "unchanged";
    for (const char* bad : {"screenshot native a b", "screenshot a native", "screenshot native ../x"}) {
        CAPTURE(bad);
        CHECK_FALSE(Ok(RunCommand(bad, game)));
    }
    CHECK(game.screenshot_name == "unchanged");
}

TEST_CASE("capture names the screenshot and the native capture alike") {
    FakeGame game;
    const std::string reply = RunCommand("capture venue_1", game);
    CHECK(Ok(reply));
    CHECK(game.capture_name == "venue_1");
    CHECK(Has(reply, "\"path\":\"screenshots/venue_1.png\""));
    CHECK(Has(reply, "\"capture\":\"screenshots/venue_1.cap\""));
    CHECK(Has(reply, "\"frame\":42"));
    CHECK(Has(reply, "\"draws\":345,\"skipped_shadow\":12,\"skipped_pass\":3"));
    CHECK(Has(reply, "\"passes\":30,\"passes_carried\":9,\"rt_sampled\":14,\"rt_missing\":1,"
                     "\"rt_filtered\":3"));
    CHECK(Has(reply, "\"rt_fallback\":\"none\""));
    CHECK(Has(reply, "\"proc_cmds\":2,\"composed\":true,\"game_frame\":2401,"
                     "\"world_frame\":2400,\"held_fallback\":false,\"emulated\":\"full\""));
    CHECK(Has(reply, "\"gpu\":\"screenshots/venue_1.gpu.png\""));
    CHECK(Has(reply, "\"gpu_ms\":4.2,\"gpu_wait_ms\":1.0"));

    CHECK(Ok(RunCommand("capture", game)));
    CHECK(game.capture_name == "");

    game.capture_name = "unchanged";
    CHECK_FALSE(Ok(RunCommand("capture ../evil", game)));
    CHECK_FALSE(Ok(RunCommand("capture a b", game)));
    CHECK(game.capture_name == "unchanged");
    CHECK_FALSE(Ok(RunCommand("p2 capture", game)));
}

TEST_CASE("capture says when the emulated GPU's picture of the frame is stale") {
    FakeGame game;
    game.capture_emulated = "stale";
    const std::string reply = RunCommand("capture", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"held_fallback\":false,\"emulated\":\"stale\""));
    CHECK(Has(reply, "\"emulated\":\"stale\",\"emulated_passes_dropped\":0"));
    // swap_only skipped passes RB3 draws once: the screenshot may lack them
    game.capture_passes_dropped = 7;
    CHECK(Has(RunCommand("capture", game), "\"emulated_passes_dropped\":7"));
}

TEST_CASE("capture with composed fails unless the capture is a composed post frame") {
    FakeGame game;
    CHECK(Ok(RunCommand("capture eo_1 composed", game)));
    CHECK(game.capture_name == "eo_1");

    // the capture fell back to a world frame of its own: written, but a failure
    game.capture_composed = false;
    game.capture_proc_cmds = 1;
    game.capture_fell_back = true;
    std::string reply = RunCommand("capture eo_2 composed", game);
    CHECK_FALSE(Ok(reply));
    CHECK(game.capture_name == "eo_2");
    CHECK(Has(reply, "proc_cmds 1, composed false, held_fallback true"));
    // without composed the same capture is fine, and says it fell back
    reply = RunCommand("capture eo_3", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"held_fallback\":true"));

    // a whole frame (even/odd rendering off) isn't a composed one either
    game.capture_proc_cmds = 7;
    game.capture_fell_back = false;
    CHECK_FALSE(Ok(RunCommand("capture eo_4 composed", game)));

    game.capture_name = "unchanged";
    CHECK_FALSE(Ok(RunCommand("capture eo_5 composd", game)));
    CHECK_FALSE(Ok(RunCommand("capture eo_5 composed x", game)));
    CHECK_FALSE(Ok(RunCommand("capture ../evil composed", game)));
    CHECK(game.capture_name == "unchanged");
}

TEST_CASE("capture still succeeds without the GPU picture, and says why") {
    FakeGame game;
    game.gpu_works = false;
    const std::string reply = RunCommand("capture venue_1", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"capture\":\"screenshots/venue_1.cap\""));
    CHECK(Has(reply, "\"gpu_error\":\"no GPU device\""));
    CHECK_FALSE(Has(reply, "\"gpu\":"));
}

TEST_CASE("native_view on starts the live view at a size, 1280x720 without one") {
    FakeGame game;
    std::string reply = RunCommand("native_view on", game);
    CHECK(Ok(reply));
    CHECK(game.view.on);
    CHECK(game.view.width == 1280);
    CHECK(game.view.height == 720);
    CHECK(Has(reply, "\"stats\":{\"on\":true,\"backend\":\"gpu\",\"width\":1280,\"height\":720,"
                     "\"post\":true"));

    CHECK(Ok(RunCommand("native_view on 640x360", game)));
    CHECK(game.view.width == 640);
    CHECK(game.view.height == 360);

    // without post-processing, at a size or the default
    reply = RunCommand("native_view on 640x360 nopost", game);
    CHECK(Ok(reply));
    CHECK(game.view.width == 640);
    CHECK_FALSE(game.view.post);
    CHECK(Has(reply, "\"height\":360,\"post\":false"));
    CHECK(Ok(RunCommand("native_view on nopost", game)));
    CHECK(game.view.width == 1280);
    CHECK_FALSE(game.view.post);
    CHECK(Ok(RunCommand("native_view on 640x360", game)));
    CHECK(game.view.post);

    for (const char* bad : {"native_view on 640", "native_view on 0x0", "native_view on x720",
                            "native_view on 640x360x2", "native_view on 640x360 more",
                            "native_view on nopost 640x360", "native_view on 640x360 nopost more",
                            "native_view on 640x360 more nopost",
                            "native_view", "native_view sideways", "native_view stats now"}) {
        CAPTURE(bad);
        CHECK_FALSE(Ok(RunCommand(bad, game)));
    }
    CHECK(game.view.width == 640);

    reply = RunCommand("native_view on 5000x720", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "no GPU target that big"));
    CHECK_FALSE(Ok(RunCommand("p2 native_view on", game)));

    // while the native renderer draws the window, its size is the window's:
    // on measures it, on at a size is an error
    game.native = true;
    CHECK(Ok(RunCommand("native_view on", game)));
    CHECK(Ok(RunCommand("native_view on nopost", game)));
    reply = RunCommand("native_view on 640x360", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "renderer is native"));
}

TEST_CASE("native_view stats reports what the live view drew and how long it took") {
    FakeGame game;
    REQUIRE(Ok(RunCommand("native_view on 1280x720", game)));
    game.view.seconds = 40;
    game.view.game_frames = 2392;
    game.view.captured = 2392;
    game.view.rendered = 20;
    game.view.skipped_busy = 2372;
    game.view.worldless = 1;
    // 1 to 20 ms: the median is 10, the 95th percentile 19
    for (int ms = 20; ms >= 1; ms--) game.view.frame_ms.push_back(ms);
    game.view.wait_ms.assign(20, 1.5);
    game.view.in_flight_max = 2;
    game.view.rt_on = true;
    game.view.rt_passes = 1200;
    game.view.rt_recorded = 3;
    game.view.rt_draws = 11;
    game.view.rt_ms = 0.25;
    const std::string reply = RunCommand("native_view stats", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"seconds\":40.0,\"game_frames\":2392,\"game_fps\":59.8"));
    CHECK(Has(reply, "\"captured\":2392,\"rendered\":20,\"skipped_busy\":2372,\"worldless\":1"));
    CHECK(Has(reply, "\"ms\":{\"mean\":10.50,\"p50\":10.00,\"p95\":19.00,\"max\":20.00}"));
    CHECK(Has(reply, "\"wait_ms\":{\"mean\":1.50,\"p50\":1.50,\"p95\":1.50,\"max\":1.50},"
                     "\"in_flight_max\":2"));
    CHECK(Has(reply, "\"rt_recording\":{\"on\":true,\"passes\":1200,\"recorded\":3,\"draws\":11,\"ms\":0.25}"));
    // not minimized
    CHECK(Has(reply, "\"in_flight_max\":2,\"paused\":false,\"paused_ms\":0.0,\"paused_captures\":0,"));
}

TEST_CASE("native_view stats reports the native renderer's pause while minimized") {
    FakeGame game;
    REQUIRE(Ok(RunCommand("native_view on", game)));
    game.view.paused = true;
    game.view.paused_ms = 5012.4;
    game.view.paused_captures = 301;
    CHECK(Has(RunCommand("native_view stats", game),
              "\"paused\":true,\"paused_ms\":5012.4,\"paused_captures\":301"));
}

TEST_CASE("native_view stats reports the frames drawn by kind, per frame drawn") {
    FakeGame game;
    REQUIRE(Ok(RunCommand("native_view on", game)));
    // off: none
    CHECK(Has(RunCommand("native_view stats", game), "\"by_kind\":{}}"));
    NativeViewStats::Kind post;
    post.name = "post";
    post.rendered = 4;
    post.skipped_busy = 3;
    post.ms = {2, 4, 6, 20};
    post.wait_ms = {1, 1, 1, 9};
    post.parts_ms = {{"plan", 8.0}, {"upload", 20.0}};
    post.counts = {{"mesh_bytes", 4096.0}};
    post.capture_ms = {{"mesh", 2.0}, {"present", 2.0}};
    post.capture_counts = {{"new_shades", 40.0}};
    post.peak = {{"meshes", 3000.0}};
    NativeViewStats::Kind between;
    between.name = "between";
    game.view.by_kind = {post, between};
    const std::string reply = RunCommand("native_view stats", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"by_kind\":{\"post\":{\"rendered\":4,\"skipped_busy\":3,"
                     "\"ms\":{\"mean\":8.00,"));
    CHECK(Has(reply, "\"parts_ms_per_frame\":{\"plan\":2.000,\"upload\":5.000},"
                     "\"per_frame\":{\"mesh_bytes\":1024.000},"
                     "\"capture\":{\"ms_per_frame\":{\"total\":1.000,\"mesh\":0.500,"
                     "\"present\":0.500},\"per_frame\":{\"new_shades\":10.000}},"
                     "\"peak\":{\"meshes\":3000.0}}"));
    // a kind with no frames: zeros, not a division by zero
    CHECK(Has(reply, ",\"between\":{\"rendered\":0,\"skipped_busy\":0,"));
    CHECK(Has(reply, "\"capture\":{\"ms_per_frame\":{\"total\":0.000},\"per_frame\":{}},"
                     "\"peak\":{}}}"));
}

TEST_CASE("native_view stats reports what capture cost the game's thread per frame") {
    FakeGame game;
    REQUIRE(Ok(RunCommand("native_view on", game)));
    NativeViewStats::Capture& c = game.view.capture;
    c.frames = 400;
    c.captured = 398;
    c.hooks_ms = {{"mesh", 800.0}, {"present", 200.0}};
    c.draws = 100000;
    c.steps = true;
    c.steps_ms = {{"bones", 300.0}, {"rest", 4.0}};
    c.counts = {{"new_shades", 120000}};
    c.sizes = {{"rts", 12}};
    const std::string reply = RunCommand("native_view stats", game);
    CHECK(Ok(reply));
    // 1000 ms over 400 frames, and 10 us over each of 100000 draws
    CHECK(Has(reply, "\"capture\":{\"frames\":400,\"captured\":398,\"ms_per_frame\":{"
                     "\"total\":2.500,\"mesh\":2.000,\"present\":0.500},"
                     "\"draws_per_frame\":250.0,\"us_per_draw\":10.00,\"steps\":true,"
                     "\"steps_ms_per_frame\":{\"bones\":0.750,\"rest\":0.010},"
                     "\"per_frame\":{\"new_shades\":300.0},\"sizes\":{\"rts\":12}}"));

    // no frames: zeros, not a division by zero
    game.view.capture = NativeViewStats::Capture{};
    CHECK(Has(RunCommand("native_view stats", game),
              "\"capture\":{\"frames\":0,\"captured\":0,\"ms_per_frame\":{\"total\":0.000},"
              "\"draws_per_frame\":0.0,\"us_per_draw\":0.00"));
}

TEST_CASE("native_view stats reports what the emulated GPU was sent per frame") {
    FakeGame game;
    NativeViewStats::EmulatedGpu& e = game.view.emulated_gpu;
    e.mode = "skip_draws";
    e.skip_mode = true;
    e.skipping = true;
    e.fresh = false;
    e.frames = 200;
    e.frames_skipped = 198;
    e.emitted = {{"begin_indexed", 5400}, {"indexed", 300}, {"instanced", 0}, {"up", 2000},
                 {"clear", 1000}, {"resolve", 600}};
    e.skipped = {{"begin_indexed", 0}, {"indexed", 80000}, {"instanced", 4000}, {"up", 6000},
                 {"clear", 0}, {"resolve", 0}};
    e.kept_pass = 300;
    e.kept_point_tests = 1800;
    e.cp_ms = 50;
    const std::string reply = RunCommand("native_view stats", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"emulated_gpu\":{\"mode\":\"skip_draws\",\"skip_mode\":true,"
                     "\"skipping\":true,\"fresh\":false,"
                     "\"frames\":200,\"frames_skipped\":198,"
                     "\"emitted_per_frame\":{\"begin_indexed\":27.0,\"indexed\":1.5,"
                     "\"instanced\":0.0,\"up\":10.0,\"clear\":5.0,\"resolve\":3.0},"
                     "\"skipped_per_frame\":{\"begin_indexed\":0.0,\"indexed\":400.0,"
                     "\"instanced\":20.0,\"up\":30.0,\"clear\":0.0,\"resolve\":0.0},"
                     "\"kept_per_frame\":{\"pass\":1.5,\"point_tests\":9.0},"
                     "\"passes_dropped\":0,\"cp_ms_per_frame\":0.250}"));

    // swap_only: the passes it dropped, a count rather than per frame
    e.mode = "swap_only";
    e.passes_dropped = 12;
    CHECK(Has(RunCommand("native_view stats", game),
              "\"emulated_gpu\":{\"mode\":\"swap_only\",\"skip_mode\":true,"));
    CHECK(Has(RunCommand("native_view stats", game), "\"passes_dropped\":12,"));

    // no frames: zeros, not a division by zero; the CP's time unknown is -1
    game.view.emulated_gpu = NativeViewStats::EmulatedGpu{};
    game.view.emulated_gpu.emitted = {{"indexed", 7}};
    const std::string none = RunCommand("native_view stats", game);
    CHECK(Has(none, "\"emulated_gpu\":{\"mode\":\"full\",\"skip_mode\":false,\"skipping\":false,"
                    "\"fresh\":true,\"frames\":0,\"frames_skipped\":0,"
                    "\"emitted_per_frame\":{\"indexed\":0.0}"));
    CHECK(Has(none, "\"cp_ms_per_frame\":-1.000}"));
    // known but no frames: 0
    game.view.emulated_gpu.cp_ms = 3;
    CHECK(Has(RunCommand("native_view stats", game), "\"cp_ms_per_frame\":0.000}"));
}

TEST_CASE("native_view stats says when there's no emulated GPU, and what the sync GPU did") {
    FakeGame game;
    NativeViewStats::EmulatedGpu& e = game.view.emulated_gpu;
    // with the emulated GPU, as before: no `present`, no `sync`
    CHECK_FALSE(Has(RunCommand("native_view stats", game), "\"present\""));
    CHECK_FALSE(Has(RunCommand("native_view stats", game), "\"sync\""));

    e.mode = "swap_only";
    e.frames = 100;
    e.present = false;
    NativeViewStats::EmulatedGpu::Sync& s = e.sync;
    s.packets = 25000;
    s.opcodes = {{"SET_CONSTANT", 9000}, {"DRAW_INDX", 50}};
    s.draws_skipped = 50;
    s.waits = 300;
    s.stalled_waits = 100;
    s.wait_ms = 400;
    s.wait_max_ms = 16.5;
    s.interrupts = 100;
    s.swaps = 100;
    s.vblanks = 100;
    s.fences = 250;
    s.zpd_writes = 40;
    s.stalled_by_band = {{"yield", 90, 300.5, 120000}, {"sleep", 10, 99.5, 12}};
    s.wait_intervals = {{"0x20", 90}, {"0x100", 10}};
    s.thread_ms = 30;
    s.vblank_thread_ms = 5;
    const std::string reply = RunCommand("native_view stats", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"cp_ms_per_frame\":-1.000,\"present\":false,\"sync\":{"
                     "\"packets_per_frame\":250.0,"
                     "\"opcodes_per_frame\":{\"SET_CONSTANT\":90.0,\"DRAW_INDX\":0.5},"
                     "\"draws_skipped_per_frame\":0.5,\"waits_per_frame\":3.0,"
                     "\"stalled_waits\":100,\"wait_ms_per_frame\":4.000,\"wait_max_ms\":16.500,"
                     "\"stalled_by_interval\":{"
                     "\"yield\":{\"waits\":90,\"ms\":300.500,\"polls\":120000},"
                     "\"sleep\":{\"waits\":10,\"ms\":99.500,\"polls\":12}},"
                     "\"wait_intervals\":{\"0x20\":90,\"0x100\":10},"
                     "\"interrupts\":100,\"swaps\":100,\"vblanks\":100,\"fences_per_frame\":2.5,"
                     "\"zpd_writes\":40,\"unknown_opcodes\":0,\"unknown_registers\":0,"
                     "\"bad_packets\":0,\"bad_addresses\":0,\"sync_ms_per_frame\":0.300,"
                     "\"vblank_ms_per_frame\":0.050}}"));

    // the threads' time unknown, and no frames: -1, and zeros
    s.thread_ms = -1;
    s.vblank_thread_ms = -1;
    s.stalled_by_band.clear();
    s.wait_intervals.clear();
    e.frames = 0;
    const std::string none = RunCommand("native_view stats", game);
    CHECK(Has(none, "\"packets_per_frame\":0.0,"));
    CHECK(Has(none, "\"stalled_by_interval\":{},\"wait_intervals\":{},"));
    CHECK(Has(none, "\"sync_ms_per_frame\":-1.000,\"vblank_ms_per_frame\":-1.000}}"));
}

TEST_CASE("native_view off reports the run it ends, then measures the game without it") {
    FakeGame game;
    REQUIRE(Ok(RunCommand("native_view on", game)));
    game.view.rendered = 7;
    game.view.frame_ms = {4.0};
    const std::string reply = RunCommand("native_view off", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"on\":true"));
    CHECK(Has(reply, "\"rendered\":7"));
    CHECK_FALSE(game.view.on);

    // nothing drawn yet: zeros, not a division by zero
    const std::string stats = RunCommand("native_view stats", game);
    CHECK(Has(stats, "\"on\":false,\"backend\":\"\",\"width\":0"));
    CHECK(Has(stats, "\"game_fps\":0.0"));
    CHECK(Has(stats, "\"ms\":{\"mean\":0.00,\"p50\":0.00,\"p95\":0.00,\"max\":0.00}"));
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

TEST_CASE("cvar reports a setting's value and what set it") {
    FakeGame game;
    CHECK(RunCommand("cvar lang", game) ==
          "{\"ok\":true,\"cvar\":{\"name\":\"lang\",\"value\":\"fre\",\"source\":\"config\"}}");
    CHECK(Has(RunCommand("cvar username", game), "\"value\":\"Some \\\"Name\\\"\""));

    const std::string reply = RunCommand("cvar nothing", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "no setting nothing"));
    CHECK_FALSE(Ok(RunCommand("cvar", game)));
    CHECK_FALSE(Ok(RunCommand("cvar lang username", game)));
    CHECK_FALSE(Ok(RunCommand("p2 cvar lang", game)));
}

TEST_CASE("folders reports the folders the game runs with") {
    FakeGame game;
    game.folders = {"C:/Games/rb3", "C:\\Users\\me\\band3", "D:/cache", {"C:/songs", "D:/more"}};
    CHECK(RunCommand("folders", game) ==
          "{\"ok\":true,\"folders\":{\"game_data\":\"C:/Games/rb3\","
          "\"user_data\":\"C:\\\\Users\\\\me\\\\band3\",\"cache\":\"D:/cache\","
          "\"content\":[\"C:/songs\",\"D:/more\"]}}");

    game.folders.content.clear();
    CHECK(Has(RunCommand("folders", game), "\"content\":[]"));
    CHECK_FALSE(Ok(RunCommand("folders now", game)));
}

TEST_CASE("bind presses a key bind, with or without its bind_ prefix") {
    FakeGame game;
    CHECK(Ok(RunCommand("bind instrument_lab", game)));
    CHECK(Ok(RunCommand("bind bind_settings", game)));
    CHECK(game.binds_pressed == std::vector<std::string>{"bind_instrument_lab", "bind_settings"});

    const std::string reply = RunCommand("bind nothing", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "no key bind bind_nothing"));
    CHECK_FALSE(Ok(RunCommand("bind", game)));
    CHECK_FALSE(Ok(RunCommand("p2 bind settings", game)));
}

TEST_CASE("liveless_invite accepts an invite to a game, on 9103 unless given a port") {
    FakeGame game;
    CHECK(Ok(RunCommand("liveless_invite 127.0.0.1", game)));
    CHECK(Ok(RunCommand("liveless_invite 192.168.1.20:9203 force_flag", game)));
    REQUIRE(game.invites.size() == 2);
    CHECK(game.invites[0].host == "127.0.0.1");
    CHECK(game.invites[0].port == 9103);
    CHECK_FALSE(game.invites[0].force_flag);
    CHECK(game.invites[1].host == "192.168.1.20");
    CHECK(game.invites[1].port == 9203);
    CHECK(game.invites[1].force_flag);

    const std::string refused = RunCommand("liveless_invite offline", game);
    CHECK_FALSE(Ok(refused));
    CHECK(Has(refused, "liveless is off"));
    CHECK(Has(RunCommand("liveless_invite host:0", game), "isn't an address"));
    CHECK_FALSE(Ok(RunCommand("liveless_invite", game)));
    CHECK_FALSE(Ok(RunCommand("liveless_invite 127.0.0.1 force", game)));
    CHECK_FALSE(Ok(RunCommand("p2 liveless_invite 127.0.0.1", game)));
    CHECK(game.invites.size() == 2);
}

namespace {

// logged in to a Rooms server on this PC, and joined a game at 192.168.1.2
band3::rooms::Status LoggedInRooms() {
    band3::rooms::Status rooms;
    rooms.state = band3::rooms::State::kLoggedIn;
    rooms.server = "127.0.0.1";
    rooms.code = "HOST0001";
    rooms.public_ipv4 = 0x0100007F;
    rooms.advertised_ipv4 = 0x0100007F;
    rooms.last_join_user = "host";
    rooms.last_join_ipv4 = 0x0201A8C0;
    rooms.game_socket_seen = true;
    rooms.attempt = 1;
    return rooms;
}

}  // namespace

TEST_CASE("rooms_status reports Liveless Rooms' status, addresses dotted") {
    FakeGame game;
    const std::string off = RunCommand("rooms_status", game);
    CHECK(off ==
          "{\"ok\":true,\"rooms\":{\"state\":\"off\",\"server\":\"\",\"code\":\"\",\"public_ip\":\"\","
          "\"advertised_ip\":\"\",\"error\":\"\",\"last_join_user\":\"\",\"last_join_ip\":\"\","
          "\"game_socket\":false,\"retry_in\":0,\"attempt\":0}}");
    game.rooms = LoggedInRooms();
    game.rooms.error = "no game with code \"NOPE0000\"";
    CHECK(RunCommand("rooms_status", game) ==
          "{\"ok\":true,\"rooms\":{\"state\":\"logged_in\",\"server\":\"127.0.0.1\",\"code\":\"HOST0001\","
          "\"public_ip\":\"127.0.0.1\",\"advertised_ip\":\"127.0.0.1\","
          "\"error\":\"no game with code \\\"NOPE0000\\\"\",\"last_join_user\":\"host\","
          "\"last_join_ip\":\"192.168.1.2\",\"game_socket\":true,\"retry_in\":0,\"attempt\":1}}");
    CHECK_FALSE(Ok(RunCommand("p2 rooms_status", game)));
}

TEST_CASE("rooms_status says when the client connects again, and which connection it's on") {
    FakeGame game;
    game.rooms = LoggedInRooms();
    game.rooms.state = band3::rooms::State::kDisconnected;
    game.rooms.error = "the server closed the connection";
    game.rooms.retry_in_s = 5;
    CHECK(Has(RunCommand("rooms_status", game), "\"retry_in\":5,\"attempt\":1}"));
    CHECK(Ok(RunCommand("rooms_status state=disconnected retry_in!=0 attempt=1", game)));
    CHECK(Ok(RunCommand("rooms_status retry_in=5 code!=HOST0002", game)));
    const std::string waiting = RunCommand("rooms_status retry_in=0", game);
    CHECK_FALSE(Ok(waiting));
    CHECK(Has(waiting, "rooms retry_in is \\\"5\\\", not \\\"0\\\""));
    game.rooms = LoggedInRooms();
    game.rooms.attempt = 2;
    CHECK(Ok(RunCommand("rooms_status state=logged_in retry_in=0 attempt=2", game)));
    const std::string not_waiting = RunCommand("rooms_status retry_in!=0", game);
    CHECK_FALSE(Ok(not_waiting));
    CHECK(Has(not_waiting, "rooms retry_in is \\\"0\\\", not other than \\\"0\\\""));
    CHECK(Has(RunCommand("rooms_status !=0", game), "usage: rooms_status"));
    CHECK(Has(RunCommand("rooms_status colour!=red", game), "rooms_status has no field colour"));
}

TEST_CASE("rooms_status checks fields, exactly or by what they contain") {
    FakeGame game;
    game.rooms = LoggedInRooms();
    game.rooms.error = "no game with code NOPE0000";
    CHECK(Ok(RunCommand("rooms_status state=logged_in code=HOST0001 public_ip=127.0.0.1", game)));
    CHECK(Ok(RunCommand("rooms_status error~NOPE0000 game_socket=true advertised_ip=127.0.0.1", game)));
    CHECK(Ok(RunCommand("rooms_status last_join_user=host last_join_ip=192.168.1.2", game)));
    const std::string wrong = RunCommand("rooms_status state=logged_in code=JOIN0001", game);
    CHECK_FALSE(Ok(wrong));
    CHECK(Has(wrong, "rooms code is \\\"HOST0001\\\", not \\\"JOIN0001\\\""));
    // the whole status comes with it
    CHECK(Has(wrong, "public_ip"));
    CHECK_FALSE(Ok(RunCommand("rooms_status error~EXIT0000", game)));
    CHECK(Has(RunCommand("rooms_status colour=red", game), "rooms_status has no field colour"));
    CHECK(Has(RunCommand("rooms_status logged_in", game), "usage: rooms_status"));
    CHECK(Has(RunCommand("rooms_status =x", game), "usage: rooms_status"));
    // an empty value is a field with nothing in it
    game.rooms.error.clear();
    CHECK(Ok(RunCommand("rooms_status error=", game)));
}

TEST_CASE("rooms_join asks for a game by code, in upper case") {
    FakeGame game;
    game.rooms = LoggedInRooms();
    const std::string reply = RunCommand("rooms_join host0001", game);
    CHECK(reply == "{\"ok\":true,\"code\":\"HOST0001\"}");
    REQUIRE(game.rooms_joins.size() == 1);
    CHECK(game.rooms_joins[0] == "HOST0001");

    CHECK(RunCommand("rooms_join lcmee", game) == "{\"ok\":true,\"code\":\"LCMEE\"}");
    REQUIRE(game.rooms_joins.size() == 2);
    CHECK(game.rooms_joins[1] == "LCMEE");
    CHECK(Has(RunCommand("rooms_join HOST00011", game), "a code is 1-8 letters and digits"));
    CHECK(Has(RunCommand("rooms_join HOST-001", game), "a code is 1-8 letters and digits"));
    CHECK(Has(RunCommand("rooms_join", game), "usage: rooms_join <code>"));
    CHECK(Has(RunCommand("rooms_join HOST0001 JOIN0001", game), "usage: rooms_join <code>"));
    game.rooms.state = band3::rooms::State::kDisconnected;
    CHECK(Has(RunCommand("rooms_join HOST0001", game), "not logged in to the Rooms server"));
    CHECK(game.rooms_joins.size() == 2);
}

TEST_CASE("rooms_join says so when the game isn't online") {
    FakeGame game;
    game.rooms = LoggedInRooms();
    game.rooms.game_socket_seen = false;
    const std::string reply = RunCommand("rooms_join HOST0001", game);
    CHECK_FALSE(Ok(reply));
    CHECK(Has(reply, "the game isn't online yet: Play on Xbox Live first"));
    CHECK(game.rooms_joins.empty());
}

TEST_CASE("rooms_connect connects again") {
    FakeGame game;
    CHECK(Has(RunCommand("rooms_connect", game), "Liveless Rooms isn't running"));
    game.rooms.state = band3::rooms::State::kFailed;
    CHECK(Ok(RunCommand("rooms_connect", game)));
    CHECK(game.rooms_connects == 1);
    CHECK(Has(RunCommand("rooms_connect now", game), "usage: rooms_connect"));
}

TEST_CASE("wait rooms= waits for the Rooms connection to reach a state") {
    FakeGame game;
    game.state.rooms_state = "connecting";
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 2s) g.state.rooms_state = "logged_in";
    };
    const std::string reply = RunCommand("wait rooms=logged_in timeout=10s", game);
    CHECK(Ok(reply));
    CHECK(game.slept >= 2s);
    CHECK(game.slept < 3s);
    // the state says it, once it's on
    CHECK(Has(reply, "\"rooms\":\"logged_in\""));
    CHECK(Ok(RunCommand("expect rooms=logged_in", game)));
    CHECK_FALSE(Ok(RunCommand("expect rooms=failed timeout=1s", game)));

    CHECK(Has(RunCommand("wait rooms=online", game), "rooms= takes off, connecting"));
    CHECK(Has(RunCommand("wait rooms=", game), "rooms= takes off, connecting"));
    FakeGame off;
    CHECK(Ok(RunCommand("expect rooms=off", off)));
    CHECK_FALSE(Has(RunCommand("state", off), "\"rooms\""));
}

TEST_CASE("port_mapping_status reports the router's mapping, and checks its fields") {
    FakeGame game;
    game.port_mapping.error = "skipped under the test harness";
    CHECK(RunCommand("port_mapping_status", game) ==
          "{\"ok\":true,\"port_mapping\":{\"state\":\"off\",\"method\":\"\",\"external_ip\":\"\","
          "\"port\":0,\"lease_s\":0,\"error\":\"skipped under the test harness\"}}");
    CHECK(Ok(RunCommand("port_mapping_status state=off error~harness", game)));

    game.port_mapping = {};
    game.port_mapping.state = band3::port_mapping::State::kMapped;
    game.port_mapping.method = band3::port_mapping::Method::kNatPmp;
    game.port_mapping.external_ipv4 = 0x057100CB;  // 203.0.113.5
    game.port_mapping.port = 9103;
    game.port_mapping.lease_s = 3600;
    CHECK(RunCommand("port_mapping_status", game) ==
          "{\"ok\":true,\"port_mapping\":{\"state\":\"mapped\",\"method\":\"natpmp\","
          "\"external_ip\":\"203.0.113.5\",\"port\":9103,\"lease_s\":3600,\"error\":\"\"}}");
    CHECK(Ok(RunCommand("port_mapping_status state=mapped method=natpmp external_ip=203.0.113.5 "
                        "port=9103 lease_s=3600",
                        game)));
    const std::string wrong = RunCommand("port_mapping_status method=pcp", game);
    CHECK_FALSE(Ok(wrong));
    CHECK(Has(wrong, "port_mapping method is \\\"natpmp\\\", not \\\"pcp\\\""));
    CHECK(Has(RunCommand("port_mapping_status lease", game), "usage: port_mapping_status"));
    CHECK(Has(RunCommand("port_mapping_status colour=red", game),
              "port_mapping_status has no field colour"));
    CHECK_FALSE(Ok(RunCommand("p2 port_mapping_status", game)));
}

TEST_CASE("port_mapping_status checks a field isn't a value") {
    FakeGame game;
    game.port_mapping.state = band3::port_mapping::State::kMapped;
    game.port_mapping.method = band3::port_mapping::Method::kUpnp;
    game.port_mapping.port = 9103;
    // a permanent UPnP mapping has lease 0; a leased one anything else
    CHECK(Ok(RunCommand("port_mapping_status state!=failed method=upnp lease_s=0 error!=x", game)));
    const std::string leased = RunCommand("port_mapping_status lease_s!=0", game);
    CHECK_FALSE(Ok(leased));
    CHECK(Has(leased, "port_mapping lease_s is \\\"0\\\", not other than \\\"0\\\""));
    game.port_mapping.lease_s = 3600;
    CHECK(Ok(RunCommand("port_mapping_status lease_s!=0", game)));
    CHECK(Has(RunCommand("port_mapping_status !=0", game), "usage: port_mapping_status"));
    CHECK(Has(RunCommand("port_mapping_status colour!=red", game),
              "port_mapping_status has no field colour"));
}

TEST_CASE("wait port_mapping= waits for the router's mapping to reach a state") {
    FakeGame game;
    CHECK(Ok(RunCommand("expect port_mapping=off", game)));
    CHECK_FALSE(Has(RunCommand("state", game), "\"port_mapping\""));
    game.state.port_mapping_state = "searching";
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 2s) g.state.port_mapping_state = "mapped";
    };
    const std::string reply = RunCommand("wait port_mapping=mapped timeout=10s", game);
    CHECK(Ok(reply));
    CHECK(game.slept >= 2s);
    CHECK(Has(reply, "\"port_mapping\":\"mapped\""));
    CHECK_FALSE(Ok(RunCommand("expect port_mapping=failed timeout=1s", game)));
    CHECK(Has(RunCommand("wait port_mapping=open", game), "port_mapping= takes off, searching"));
}

TEST_CASE("wait joined waits for an online band to form with the game in it") {
    FakeGame game;
    CHECK_FALSE(Has(RunCommand("state", game), "\"joined\""));
    CHECK_FALSE(Ok(RunCommand("expect joined timeout=1s", game)));
    game.on_sleep = [](FakeGame& g) {
        if (g.slept >= 2s) g.state.joined = true;
    };
    const std::string reply = RunCommand("wait joined timeout=10s", game);
    CHECK(Ok(reply));
    CHECK(game.slept >= 2s);
    CHECK(game.slept < 3s);
    CHECK(Has(reply, "\"joined\":true"));
    CHECK(Has(RunCommand("state", game), "\"joined\":true"));
    CHECK(Has(RunCommand("wait joined=yes", game), "no condition joined=yes"));
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
    state.SetScore(4200);
    state.SetInGame(false);
    CHECK(state.Snapshot().song_shortname == "ruby");
    CHECK(state.Snapshot().score == 4200);
    // the next song starts its score over
    state.SetInGame(true);
    CHECK(state.Snapshot().score == 0);
}

TEST_CASE("the game state follows the song's position, unknown until it's read") {
    GameState state;
    state.SetSong("Ruby", "Kaiser Chiefs", "ruby", 210000);
    state.SetInGame(true);
    CHECK(state.InGame());
    CHECK(state.Snapshot().song_length_ms == 210000);
    CHECK(state.Snapshot().song_ms == -1);
    state.SetSongTime(61000);
    CHECK(state.Snapshot().song_ms == 61000);
    // a new song starts unknown again
    state.SetInGame(false);
    CHECK(!state.InGame());
    state.SetInGame(true);
    CHECK(state.Snapshot().song_ms == -1);
}

TEST_CASE("the game state keeps that an online band formed, through songs") {
    GameState state;
    CHECK_FALSE(state.Snapshot().joined);
    state.SetJoined();
    state.SetInGame(true);
    state.SetInGame(false);
    CHECK(state.Snapshot().joined);
}

TEST_CASE("present_stats reports the window's paints, the native frames and the game's") {
    FakeGame game;
    game.present.renderer = "native";
    game.present.path = "zero-copy";
    game.present.seconds = 20;
    game.present.paints = 1200;
    // 16 ms, but two at 40: hitches, longer than 1.5 times the median
    game.present.paint_ms.assign(18, 16.0);
    game.present.paint_ms.push_back(40.0);
    game.present.paint_ms.push_back(40.0);
    game.present.native_paints = 1200;
    game.present.shown = 1190;
    game.present.repeats = 10;
    game.present.skipped = 3;
    game.present.latency_ms = {20.0, 30.0};
    game.present.publish_latency_ms = {8.0, 12.0, 10.0};
    game.present.game_frames = 1196;
    game.present.game_ms.assign(20, 16.7);
    game.present.cap = {.mode = "display", .hz = 119.88, .late = 4, .resets = 1,
                        .wait_ms = 2.5, .spin_ms = 0.4};
    std::string reply = RunCommand("present_stats", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"renderer\":\"native\",\"path\":\"zero-copy\""));
    CHECK(Has(reply, "\"seconds\":20.0,\"paints\":1200,\"paint_fps\":60.0"));
    CHECK(Has(reply, "\"paint_ms\":{\"mean\":18.40,\"p50\":16.00,\"p95\":40.00,\"max\":40.00}"));
    CHECK(Has(reply, "\"hitches\":2,\"native\""));
    CHECK(Has(reply, "\"native\":{\"paints\":1200,\"shown\":1190,\"repeats\":10,\"skipped\":3"));
    CHECK(Has(reply, "\"latency_ms\":{\"mean\":25.00,\"p50\":20.00,\"p95\":30.00"));
    // the frames handed to the window and how long after the game's Present
    CHECK(Has(reply, "\"max\":30.00},\"published\":3,\"publish_latency_ms\":{\"mean\":10.00,"
                     "\"p50\":10.00,\"p95\":12.00,\"max\":12.00}},\"game\""));
    CHECK(Has(reply, "\"game\":{\"frames\":1196,\"fps\":59.8"));
    CHECK(Has(reply, "\"hitches\":0,\"cap\":{\"mode\":\"display\",\"hz\":119.88,\"late\":4,"
                     "\"resets\":1,\"wait_ms\":2.500,\"spin_ms\":0.400}}}"));
    CHECK(game.present_resets == 0);

    // reset replies with the stretch it ends, then starts over
    reply = RunCommand("present_stats reset", game);
    CHECK(Ok(reply));
    CHECK(Has(reply, "\"paints\":1200"));
    CHECK(game.present_resets == 1);
    CHECK(Has(RunCommand("present_stats", game), "\"paints\":0"));

    for (const char* bad : {"present_stats now", "present_stats reset more", "p2 present_stats"}) {
        CAPTURE(bad);
        CHECK_FALSE(Ok(RunCommand(bad, game)));
    }
}
