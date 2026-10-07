#include "test_commands.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>
#include "src/Net/liveless_rooms_client.h"
#include "src/Net/online.h"
#include "test_inputs.h"

namespace band3::test {

using namespace std::chrono_literals;
using input::InstrumentInputs;

namespace {

constexpr std::chrono::milliseconds kPressLength = 100ms;
// after a press lets go, so a press straight after it reads as a new one
constexpr std::chrono::milliseconds kReleaseGap = 50ms;
// the virtual instrument stays unplugged for 500 ms when it changes
constexpr std::chrono::milliseconds kReplugWait = 1000ms;
constexpr std::chrono::milliseconds kWaitTimeout = 30s;
constexpr std::chrono::milliseconds kExpectTimeout = 5s;
// how long `press ... until` waits before pressing again
constexpr std::chrono::milliseconds kPressRetry = 2s;
constexpr std::chrono::milliseconds kMaxSleep = 600s;
// about a frame
constexpr std::chrono::milliseconds kWaitPoll = 16ms;
constexpr uint8_t kDefaultVelocity = 100;
constexpr int kPlayerCount = 4;
// ExternalMic::Init makes four
constexpr int kMicSlots = 4;

std::vector<std::string_view> Words(std::string_view line) {
    std::vector<std::string_view> words;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) i++;
        const size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '\r') i++;
        if (i > start) words.push_back(line.substr(start, i - start));
    }
    return words;
}

template <typename T>
std::optional<T> ParseNumber(std::string_view text) {
    T value{};
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), value);
    if (ec != std::errc() || end != text.data() + text.size()) return std::nullopt;
    return value;
}

// "30s", "250ms"
std::optional<std::chrono::milliseconds> ParseDuration(std::string_view text) {
    if (text.ends_with("ms")) {
        auto n = ParseNumber<int64_t>(text.substr(0, text.size() - 2));
        if (n && *n >= 0) return std::chrono::milliseconds(*n);
    } else if (text.ends_with("s")) {
        auto n = ParseNumber<double>(text.substr(0, text.size() - 1));
        if (n && *n >= 0) return std::chrono::milliseconds(static_cast<int64_t>(*n * 1000));
    }
    return std::nullopt;
}

void AppendJsonString(std::string& out, std::string_view text) {
    out += '"';
    for (char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                out += escaped;
            } else {
                out += c;
            }
        }
    }
    out += '"';
}

std::string StateJson(const GameStateSnapshot& s, TestTarget& target) {
    std::string out = "{\"screen\":";
    AppendJsonString(out, s.screen);
    out += ",\"in_game\":";
    out += s.in_game ? "true" : "false";
    out += ",\"song\":{\"name\":";
    AppendJsonString(out, s.song_name);
    out += ",\"artist\":";
    AppendJsonString(out, s.song_artist);
    out += ",\"shortname\":";
    AppendJsonString(out, s.song_shortname);
    out += "},\"venue\":";
    AppendJsonString(out, s.venue);
    out += ",\"band\":[";
    for (size_t i = 0; i < s.band.size(); i++) {
        const BandMember& m = s.band[i];
        if (i) out += ',';
        out += "{\"exists\":";
        out += m.exists ? "true" : "false";
        out += ",\"difficulty\":" + std::to_string(m.difficulty);
        out += ",\"track\":" + std::to_string(m.track_type) + "}";
    }
    out += "],\"frame\":" + std::to_string(s.frame);
    out += ",\"score\":" + std::to_string(s.score);
    if (!s.mics.empty()) {
        out += ",\"mics\":[";
        for (size_t i = 0; i < s.mics.size(); i++) {
            const MicSlot& mic = s.mics[i];
            if (i) out += ',';
            out += "{\"device\":";
            AppendJsonString(out, mic.device);
            out += ",\"connected\":";
            out += mic.connected ? "true" : "false";
            out += ",\"fed\":" + std::to_string(mic.bytes_fed) + "}";
        }
        out += ']';
    }
    if (s.rooms_state != "off") {
        out += ",\"rooms\":";
        AppendJsonString(out, s.rooms_state);
    }
    if (s.port_mapping_state != "off") {
        out += ",\"port_mapping\":";
        AppendJsonString(out, s.port_mapping_state);
    }
    if (s.joined) out += ",\"joined\":true";
    out += ",\"instruments\":[";
    for (int player = 1; player <= kPlayerCount; player++) {
        if (player > 1) out += ',';
        if (auto kind = target.Kind(player)) {
            AppendJsonString(out, InstrumentName(*kind));
        } else {
            out += "null";
        }
    }
    out += "]}";
    return out;
}

std::string Error(TestTarget& target, std::string_view message) {
    std::string out = "{\"ok\":false,\"error\":";
    AppendJsonString(out, message);
    out += ",\"state\":" + StateJson(target.State(), target) + "}";
    return out;
}

std::string Ok(std::string_view fields = {}) {
    std::string out = "{\"ok\":true";
    if (!fields.empty()) {
        out += ',';
        out += fields;
    }
    out += '}';
    return out;
}

std::string OkWithState(TestTarget& target, const GameStateSnapshot& state) {
    return Ok("\"state\":" + StateJson(state, target));
}

// the player a controller command is for, and that player's instrument
struct Controller {
    int player;
    input::InstrumentKind kind;

    // an input error, naming whose instrument it is
    std::string Mention(const std::string& error) const {
        return "player " + std::to_string(player) + ": " + error;
    }
};

// applies every name in `list` to `in` at `value`; an error, or empty
std::string ApplyInputs(const Controller& c, InstrumentInputs& in, std::string_view list,
                        uint8_t value) {
    for (const std::string& name : SplitInputs(list)) {
        if (name.empty()) return "empty input name in " + std::string(list);
        std::string error = SetInput(c.kind, in, name, value);
        if (!error.empty()) return c.Mention(error);
    }
    return {};
}

// `press <inputs> [ms] [until <condition> [every=<time>] [timeout=<time>]]`.
// With `until`, it waits up to `timeout` (30 s by default) for the condition,
// and presses again each time `every` (2 s) goes by with the screen still the
// one it pressed on: RB3 drops a press made while a screen is still coming
// in, and how long that takes isn't a number of frames at every refresh rate.
// Once the screen has changed at all (a loading screen on the way, say) the
// press took, and it's never made again, so a retry can't land on the next
// screen.
std::string Press(TestTarget& target, const Controller& c,
                  const std::vector<std::string_view>& args) {
    const auto until = std::find(args.begin(), args.end(), "until");
    const size_t press_args = size_t(until - args.begin());
    const char* usage = "usage: press <inputs> [ms] [until <condition> [every=<n>s] [timeout=<n>s]]";
    if (press_args < 2 || press_args > 3) return Error(target, usage);
    std::chrono::milliseconds length = kPressLength;
    if (press_args == 3) {
        auto ms = ParseNumber<int64_t>(args[2]);
        if (!ms || *ms < 1 || *ms > 10000) return Error(target, "press length is 1 to 10000 ms");
        length = std::chrono::milliseconds(*ms);
    }
    // checked on a copy, so a bad name presses nothing
    InstrumentInputs check;
    if (std::string error = ApplyInputs(c, check, args[1], kDefaultVelocity); !error.empty())
        return Error(target, error);

    std::optional<Condition> condition;
    std::chrono::milliseconds every = kPressRetry, timeout = kWaitTimeout;
    if (until != args.end()) {
        if (until + 1 == args.end()) return Error(target, usage);
        auto parsed = ParseCondition(*(until + 1));
        if (auto* error = std::get_if<std::string>(&parsed)) return Error(target, *error);
        condition = std::get<Condition>(parsed);
        for (auto it = until + 2; it != args.end(); ++it) {
            const bool is_every = it->starts_with("every="), is_timeout = it->starts_with("timeout=");
            auto value = is_every     ? ParseDuration(it->substr(6))
                         : is_timeout ? ParseDuration(it->substr(8))
                                      : std::nullopt;
            if (!value || *value <= std::chrono::milliseconds(0))
                return Error(target, "bad " + std::string(*it) + " (" + usage + ")");
            (is_every ? every : timeout) = *value;
        }
    }

    const input::InstrumentKind kind = c.kind;
    const std::string list(args[1]);
    auto press = [&] {
        target.Pulse(c.player, [kind, list](InstrumentInputs& in) {
            for (const std::string& name : SplitInputs(list))
                SetInput(kind, in, name, kDefaultVelocity);
        }, length);
        target.Sleep(length + kReleaseGap);
    };
    if (!condition) {
        press();
        return Ok();
    }

    const auto start = target.Now();
    const GameStateSnapshot start_state = target.State();
    for (int presses = 1;; presses++) {
        press();
        const auto pressed_at = target.Now();
        GameStateSnapshot state = target.State();
        while (!ConditionHolds(*condition, state, start_state)) {
            if (target.Cancelled()) return Error(target, "the test server is shutting down");
            if (target.Now() - start >= timeout) {
                return Error(target, "timed out after " + std::to_string(timeout.count()) +
                                         " ms and " + std::to_string(presses) +
                                         (presses == 1 ? " press" : " presses") +
                                         " waiting for " + std::string(*(until + 1)));
            }
            if (state.screen == start_state.screen && target.Now() - pressed_at >= every) break;
            target.Sleep(kWaitPoll);
            state = target.State();
        }
        if (ConditionHolds(*condition, state, start_state)) return OkWithState(target, state);
    }
}

std::string Hit(TestTarget& target, const Controller& c,
                const std::vector<std::string_view>& args) {
    if (args.size() < 2 || args.size() > 4)
        return Error(target, "usage: hit <target> [velocity] [fret]");
    int velocity = kDefaultVelocity;
    int fret = 0;
    if (args.size() >= 3) {
        auto v = ParseNumber<int>(args[2]);
        if (!v || *v < 1 || *v > 127) return Error(target, "velocity is 1 to 127");
        velocity = *v;
    }
    if (args.size() == 4) {
        auto f = ParseNumber<int>(args[3]);
        if (!f || *f < 0 || *f > 255) return Error(target, "bad fret " + std::string(args[3]));
        fret = *f;
    }
    InstrumentInputs check;
    const input::InstrumentKind kind = c.kind;
    const std::string name(args[1]);
    if (std::string error = SetInput(kind, check, name, static_cast<uint8_t>(velocity),
                                     static_cast<uint8_t>(fret));
        !error.empty())
        return Error(target, c.Mention(error));

    target.Pulse(c.player, [kind, name, velocity, fret](InstrumentInputs& in) {
        SetInput(kind, in, name, static_cast<uint8_t>(velocity), static_cast<uint8_t>(fret));
    }, kPressLength);
    target.Sleep(kPressLength + kReleaseGap);
    return Ok();
}

std::string Hold(TestTarget& target, const Controller& c,
                 const std::vector<std::string_view>& args, bool down) {
    if (args.size() != 2) {
        return Error(target, down ? "usage: hold <inputs>" : "usage: release <inputs>|all");
    }
    if (!down && args[1] == "all") {
        target.SetHeld(c.player, {});
        return Ok();
    }
    InstrumentInputs held = target.Held(c.player);
    if (std::string error = ApplyInputs(c, held, args[1], down ? kDefaultVelocity : 0);
        !error.empty())
        return Error(target, error);
    target.SetHeld(c.player, held);
    return Ok();
}

std::string Axis(TestTarget& target, const Controller& c,
                 const std::vector<std::string_view>& args) {
    if (args.size() != 3) return Error(target, "usage: axis whammy|tilt <0..1>");
    auto value = ParseNumber<float>(args[2]);
    if (!value) return Error(target, "bad axis value " + std::string(args[2]));
    InstrumentInputs held = target.Held(c.player);
    if (std::string error = SetAxis(c.kind, held, args[1], *value); !error.empty())
        return Error(target, c.Mention(error));
    target.SetHeld(c.player, held);
    return Ok();
}

std::string Instrument(TestTarget& target, int player,
                       const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: instrument guitar|drums|keys|mustang|squier");
    auto kind = ParseInstrumentName(args[1]);
    if (!kind) return Error(target, "no instrument " + std::string(args[1]));
    if (target.Kind(player) != kind) {
        target.SetHeld(player, {});
        target.Plug(player, *kind);
        // long enough for the replug and for RB3 to see it connect
        target.Sleep(kReplugWait);
    }
    return Ok();
}

std::string Unplug(TestTarget& target, int player, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return Error(target, "usage: unplug");
    target.SetHeld(player, {});
    if (target.Kind(player)) target.Unplug(player);
    return Ok();
}

std::string Wait(TestTarget& target, const std::vector<std::string_view>& args,
                 std::chrono::milliseconds timeout) {
    if (args.size() < 2 || args.size() > 3) {
        return Error(target, "usage: " + std::string(args[0]) + " <condition> [timeout=<n>s]");
    }
    auto parsed = ParseCondition(args[1]);
    if (auto* error = std::get_if<std::string>(&parsed)) return Error(target, *error);
    if (args.size() == 3) {
        auto value = args[2].starts_with("timeout=") ? ParseDuration(args[2].substr(8))
                                                     : std::nullopt;
        if (!value) return Error(target, "bad timeout " + std::string(args[2]));
        timeout = *value;
    }

    const Condition& condition = std::get<Condition>(parsed);
    const auto start = target.Now();
    const GameStateSnapshot start_state = target.State();
    GameStateSnapshot state = start_state;
    while (!ConditionHolds(condition, state, start_state)) {
        if (target.Cancelled()) return Error(target, "the test server is shutting down");
        if (target.Now() - start >= timeout) {
            return Error(target, "timed out after " + std::to_string(timeout.count()) +
                                     " ms waiting for " + std::string(args[1]));
        }
        target.Sleep(kWaitPoll);
        state = target.State();
    }
    return OkWithState(target, state);
}

// `sleep <n>s|<n>ms`: wall-clock time, for what `wait frames=` can't count.
// At boot RB3's splash thread draws the logos while the main thread loads, and
// the frame count (App::DrawRegular's) stands still until the intro movie.
std::string SleepFor(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: sleep <n>s|<n>ms");
    auto length = ParseDuration(args[1]);
    if (!length || *length > kMaxSleep)
        return Error(target, "sleep takes 0 to 600 s, as 2s or 250ms");
    target.Sleep(*length);
    if (target.Cancelled()) return Error(target, "the test server is shutting down");
    return Ok();
}

std::string Pad(TestTarget& target, std::optional<int> prefix,
                const std::vector<std::string_view>& args) {
    if (args.size() > 2) return Error(target, "usage: pad [player]");
    int player = prefix.value_or(1);
    if (args.size() == 2) {
        if (prefix) return Error(target, "pad takes a player or a prefix, not both");
        auto p = ParseNumber<int>(args[1]);
        if (!p || *p < 1 || *p > kPlayerCount) return Error(target, "player is 1 to 4");
        player = *p;
    }
    input::Gamepad360 pad;
    uint32_t packet = 0;
    if (!target.ReadPad(player, pad, packet)) return Ok("\"pad\":{\"connected\":false}");

    // XINPUT_GAMEPAD's button bits, lowest first
    static constexpr const char* kButtons[16] = {
        "up", "down", "left", "right", "start", "back", "lstick", "rstick",
        "lb", "rb", "guide", nullptr, "a", "b", "x", "y"};
    std::string out = "\"pad\":{\"connected\":true,\"buttons\":[";
    bool first = true;
    for (int bit = 0; bit < 16; bit++) {
        if (!(pad.buttons & (1u << bit)) || !kButtons[bit]) continue;
        if (!first) out += ',';
        AppendJsonString(out, kButtons[bit]);
        first = false;
    }
    out += "],\"lt\":" + std::to_string(pad.left_trigger);
    out += ",\"rt\":" + std::to_string(pad.right_trigger);
    out += ",\"lx\":" + std::to_string(pad.thumb_lx);
    out += ",\"ly\":" + std::to_string(pad.thumb_ly);
    out += ",\"rx\":" + std::to_string(pad.thumb_rx);
    out += ",\"ry\":" + std::to_string(pad.thumb_ry);
    out += ",\"packet\":" + std::to_string(packet) + "}";
    return Ok(out);
}

bool IsFileName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

// `<verb> [name]`'s name into `name`, or an error reply
std::string FileNameArg(TestTarget& target, const std::vector<std::string_view>& args,
                        std::string& name) {
    if (args.size() > 2) return Error(target, "usage: " + std::string(args[0]) + " [name]");
    if (args.size() == 2) {
        if (!IsFileName(args[1])) {
            return Error(target, "a " + std::string(args[0]) +
                                     " name is up to 64 letters, digits, _ and -");
        }
        name = args[1];
    }
    return {};
}

// `screenshot [emulated|native] [name]`: the picture the window shows, as the
// renderer setting has it, or the one named
std::string Screenshot(TestTarget& target, const std::vector<std::string_view>& args) {
    ScreenshotSource source = ScreenshotSource::kWindow;
    std::vector<std::string_view> name_args = args;
    if (args.size() >= 2 && (args[1] == "emulated" || args[1] == "native")) {
        source = args[1] == "native" ? ScreenshotSource::kNative : ScreenshotSource::kEmulated;
        name_args.erase(name_args.begin() + 1);
    }
    std::string name;
    if (name_args.size() > 2) return Error(target, "usage: screenshot [emulated|native] [name]");
    if (std::string error = FileNameArg(target, name_args, name); !error.empty()) return error;
    ScreenshotInfo info;
    if (std::string error = target.Screenshot(name, source, info); !error.empty())
        return Error(target, error);
    std::string fields = "\"path\":";
    AppendJsonString(fields, info.path);
    fields += ",\"width\":" + std::to_string(info.width);
    fields += ",\"height\":" + std::to_string(info.height);
    fields += ",\"renderer\":";
    AppendJsonString(fields, info.renderer);
    return Ok(fields);
}

// `capture [name] [composed]`: with `composed`, the reply is a failure unless
// the capture is a post frame composed with the world frame before it
// (proc_cmds 2), as every one is meant to be with even/odd rendering on; its
// files are written either way
std::string Capture(TestTarget& target, const std::vector<std::string_view>& args) {
    const bool want_composed = args.size() == 3 && args[2] == "composed";
    if (args.size() > 3 || (args.size() == 3 && !want_composed))
        return Error(target, "usage: capture [name] [composed]");
    std::vector<std::string_view> name_args = args;
    if (want_composed) name_args.pop_back();
    std::string name;
    if (std::string error = FileNameArg(target, name_args, name); !error.empty()) return error;
    CaptureInfo info;
    if (std::string error = target.Capture(name, info); !error.empty())
        return Error(target, error);
    std::string fields = "\"path\":";
    AppendJsonString(fields, info.screenshot.path);
    fields += ",\"width\":" + std::to_string(info.screenshot.width);
    fields += ",\"height\":" + std::to_string(info.screenshot.height);
    fields += ",\"capture\":";
    AppendJsonString(fields, info.capture_path);
    fields += ",\"frame\":" + std::to_string(info.frame);
    fields += ",\"draws\":" + std::to_string(info.draws);
    fields += ",\"skipped_shadow\":" + std::to_string(info.skipped_shadow);
    fields += ",\"skipped_pass\":" + std::to_string(info.skipped_pass);
    fields += ",\"passes\":" + std::to_string(info.passes);
    fields += ",\"passes_carried\":" + std::to_string(info.passes_carried);
    fields += ",\"rt_sampled\":" + std::to_string(info.rt_sampled);
    fields += ",\"rt_missing\":" + std::to_string(info.rt_missing);
    fields += ",\"rt_filtered\":" + std::to_string(info.rt_filtered);
    fields += ",\"rt_fallback\":";
    AppendJsonString(fields, info.rt_fallback);
    fields += ",\"proc_cmds\":" + std::to_string(info.proc_cmds);
    fields += std::string(",\"composed\":") + (info.composed ? "true" : "false");
    fields += ",\"game_frame\":" + std::to_string(info.game_frame);
    fields += ",\"world_frame\":" + std::to_string(info.world_frame);
    fields += std::string(",\"held_fallback\":") + (info.held_fallback ? "true" : "false");
    fields += ",\"emulated\":";
    AppendJsonString(fields, info.emulated);
    fields += ",\"emulated_passes_dropped\":" + std::to_string(info.emulated_passes_dropped);
    if (want_composed && !(info.composed && info.proc_cmds == 2)) {
        return Error(target, "capture " + name + " isn't a post frame composed with the world "
                                 "before it: proc_cmds " + std::to_string(info.proc_cmds) +
                                 ", composed " + (info.composed ? "true" : "false") +
                                 ", held_fallback " + (info.held_fallback ? "true" : "false"));
    }
    if (!info.gpu_path.empty()) {
        fields += ",\"gpu\":";
        AppendJsonString(fields, info.gpu_path);
        char ms[64];
        std::snprintf(ms, sizeof(ms), ",\"gpu_ms\":%.1f,\"gpu_wait_ms\":%.1f", info.gpu_ms,
                      info.gpu_wait_ms);
        fields += ms;
        fields += ",\"gpu_passes\":" + std::to_string(info.gpu_passes);
        fields += ",\"gpu_rt_missing\":" + std::to_string(info.gpu_rt_missing);
        if (!info.gpu_presented_path.empty()) {
            fields += ",\"gpu_presented\":";
            AppendJsonString(fields, info.gpu_presented_path);
        }
    } else if (!info.gpu_error.empty()) {
        fields += ",\"gpu_error\":";
        AppendJsonString(fields, info.gpu_error);
    }
    return Ok(fields);
}

// mean, median, 95th percentile and worst of `ms`, nearest rank
std::string Distribution(std::vector<double> ms) {
    double mean = 0, p50 = 0, p95 = 0, max = 0;
    if (!ms.empty()) {
        std::sort(ms.begin(), ms.end());
        for (double m : ms) mean += m;
        mean /= double(ms.size());
        auto rank = [&](double p) {
            const size_t i = static_cast<size_t>(std::ceil(p * double(ms.size())));
            return ms[std::clamp<size_t>(i, 1, ms.size()) - 1];
        };
        p50 = rank(0.5);
        p95 = rank(0.95);
        max = ms.back();
    }
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"mean\":%.2f,\"p50\":%.2f,\"p95\":%.2f,\"max\":%.2f}", mean,
                  p50, p95, max);
    return buf;
}

// the intervals longer than 1.5 times their median: frames that came late
uint64_t Hitches(std::vector<double> ms) {
    if (ms.empty()) return 0;
    std::sort(ms.begin(), ms.end());
    const size_t i = static_cast<size_t>(std::ceil(0.5 * double(ms.size())));
    const double median = ms[std::clamp<size_t>(i, 1, ms.size()) - 1];
    return uint64_t(std::count_if(ms.begin(), ms.end(), [&](double m) { return m > 1.5 * median; }));
}

std::string PresentJson(const PresentStats& s) {
    std::string out = "\"stats\":{\"renderer\":";
    AppendJsonString(out, s.renderer);
    out += ",\"path\":";
    AppendJsonString(out, s.path);
    const double seconds = s.seconds > 0 ? s.seconds : 0;
    char buf[96];
    std::snprintf(buf, sizeof(buf), ",\"seconds\":%.1f,\"paints\":%llu,\"paint_fps\":%.1f",
                  seconds, static_cast<unsigned long long>(s.paints),
                  seconds > 0 ? double(s.paints) / seconds : 0.0);
    out += buf;
    out += ",\"paint_ms\":" + Distribution(s.paint_ms);
    out += ",\"hitches\":" + std::to_string(Hitches(s.paint_ms));
    out += ",\"native\":{\"paints\":" + std::to_string(s.native_paints);
    out += ",\"shown\":" + std::to_string(s.shown);
    out += ",\"repeats\":" + std::to_string(s.repeats);
    out += ",\"skipped\":" + std::to_string(s.skipped);
    out += ",\"latency_ms\":" + Distribution(s.latency_ms);
    out += ",\"published\":" + std::to_string(s.publish_latency_ms.size());
    out += ",\"publish_latency_ms\":" + Distribution(s.publish_latency_ms) + "}";
    std::snprintf(buf, sizeof(buf), ",\"game\":{\"frames\":%llu,\"fps\":%.1f",
                  static_cast<unsigned long long>(s.game_frames),
                  seconds > 0 ? double(s.game_frames) / seconds : 0.0);
    out += buf;
    out += ",\"ms\":" + Distribution(s.game_ms);
    out += ",\"hitches\":" + std::to_string(Hitches(s.game_ms));
    out += ",\"cap\":{\"mode\":";
    AppendJsonString(out, s.cap.mode);
    char cap[192];
    std::snprintf(cap, sizeof(cap),
                  ",\"hz\":%.2f,\"late\":%llu,\"resets\":%llu,\"wait_ms\":%.3f,\"spin_ms\":%.3f}}}",
                  s.cap.hz, static_cast<unsigned long long>(s.cap.late),
                  static_cast<unsigned long long>(s.cap.resets), s.cap.wait_ms, s.cap.spin_ms);
    out += cap;
    return out;
}

// `present_stats [reset]`: the window's pacing since it last started over;
// reset starts it over, and replies with the stretch it ends
std::string PresentStatsCommand(TestTarget& target, const std::vector<std::string_view>& args) {
    const bool reset = args.size() == 2 && args[1] == "reset";
    if (args.size() > 2 || (args.size() == 2 && !reset))
        return Error(target, "usage: present_stats [reset]");
    return Ok(PresentJson(target.Present(reset)));
}

// native_view stats' `capture`: what capture cost the game's thread, per game
// frame (0 with no frames), and per draw recorded
std::string CaptureCostJson(const NativeViewStats::Capture& c) {
    const double frames = double(c.frames);
    auto per_frame = [&](double v) { return frames > 0 ? v / frames : 0.0; };
    char buf[96];
    double total_ms = 0;
    for (const auto& [name, ms] : c.hooks_ms) total_ms += ms;
    std::string out = "{\"frames\":" + std::to_string(c.frames);
    out += ",\"captured\":" + std::to_string(c.captured);
    std::snprintf(buf, sizeof(buf), ",\"ms_per_frame\":{\"total\":%.3f", per_frame(total_ms));
    out += buf;
    for (const auto& [name, ms] : c.hooks_ms) {
        std::snprintf(buf, sizeof(buf), ",\"%s\":%.3f", name.c_str(), per_frame(ms));
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "},\"draws_per_frame\":%.1f,\"us_per_draw\":%.2f",
                  per_frame(double(c.draws)), c.draws ? total_ms * 1000 / double(c.draws) : 0.0);
    out += buf;
    out += ",\"steps\":";
    out += c.steps ? "true" : "false";
    out += ",\"steps_ms_per_frame\":{";
    for (size_t i = 0; i < c.steps_ms.size(); i++) {
        std::snprintf(buf, sizeof(buf), "%s\"%s\":%.3f", i ? "," : "", c.steps_ms[i].first.c_str(),
                      per_frame(c.steps_ms[i].second));
        out += buf;
    }
    out += "},\"per_frame\":{";
    for (size_t i = 0; i < c.counts.size(); i++) {
        std::snprintf(buf, sizeof(buf), "%s\"%s\":%.1f", i ? "," : "", c.counts[i].first.c_str(),
                      per_frame(double(c.counts[i].second)));
        out += buf;
    }
    out += "},\"sizes\":{";
    for (size_t i = 0; i < c.sizes.size(); i++) {
        out += i ? "," : "";
        out += "\"" + c.sizes[i].first + "\":" + std::to_string(c.sizes[i].second);
    }
    out += "}}";
    return out;
}

// `emulated_gpu`'s `sync`, with renderer native: what the sync-only GPU did,
// the busy counts per game frame (0 with no frames), the rare ones as they
// are, and its threads' CPU milliseconds per frame (-1 unknown)
std::string SyncGpuJson(const NativeViewStats::EmulatedGpu::Sync& s, double frames) {
    auto per_frame = [&](double v) { return frames > 0 ? v / frames : 0.0; };
    char buf[128];
    std::snprintf(buf, sizeof(buf), "{\"packets_per_frame\":%.1f,\"opcodes_per_frame\":{",
                  per_frame(double(s.packets)));
    std::string out = buf;
    for (size_t i = 0; i < s.opcodes.size(); i++) {
        out += i ? "," : "";
        AppendJsonString(out, s.opcodes[i].first);
        std::snprintf(buf, sizeof(buf), ":%.1f", per_frame(double(s.opcodes[i].second)));
        out += buf;
    }
    std::snprintf(buf, sizeof(buf),
                  "},\"draws_skipped_per_frame\":%.1f,\"waits_per_frame\":%.1f,\"stalled_waits\":"
                  "%llu,",
                  per_frame(double(s.draws_skipped)), per_frame(double(s.waits)),
                  static_cast<unsigned long long>(s.stalled_waits));
    out += buf;
    std::snprintf(buf, sizeof(buf), "\"wait_ms_per_frame\":%.3f,\"wait_max_ms\":%.3f,",
                  per_frame(s.wait_ms), s.wait_max_ms);
    out += buf;
    // the stalled waits by their wait interval, as they are
    out += "\"stalled_by_interval\":{";
    for (size_t i = 0; i < s.stalled_by_band.size(); i++) {
        const auto& b = s.stalled_by_band[i];
        out += i ? "," : "";
        AppendJsonString(out, b.name);
        std::snprintf(buf, sizeof(buf), ":{\"waits\":%llu,\"ms\":%.3f,\"polls\":%llu}",
                      static_cast<unsigned long long>(b.waits), b.ms,
                      static_cast<unsigned long long>(b.polls));
        out += buf;
    }
    out += "},\"wait_intervals\":{";
    for (size_t i = 0; i < s.wait_intervals.size(); i++) {
        out += i ? "," : "";
        AppendJsonString(out, s.wait_intervals[i].first);
        out += ":" + std::to_string(s.wait_intervals[i].second);
    }
    out += "},\"interrupts\":" + std::to_string(s.interrupts);
    out += ",\"swaps\":" + std::to_string(s.swaps);
    out += ",\"vblanks\":" + std::to_string(s.vblanks);
    std::snprintf(buf, sizeof(buf), ",\"fences_per_frame\":%.1f", per_frame(double(s.fences)));
    out += buf;
    out += ",\"zpd_writes\":" + std::to_string(s.zpd_writes);
    out += ",\"unknown_opcodes\":" + std::to_string(s.unknown_opcodes);
    out += ",\"unknown_registers\":" + std::to_string(s.unknown_registers);
    out += ",\"bad_packets\":" + std::to_string(s.bad_packets);
    out += ",\"bad_addresses\":" + std::to_string(s.bad_addresses);
    const double ms = s.thread_ms < 0 ? -1.0 : per_frame(s.thread_ms);
    const double vblank_ms = s.vblank_thread_ms < 0 ? -1.0 : per_frame(s.vblank_thread_ms);
    std::snprintf(buf, sizeof(buf), ",\"sync_ms_per_frame\":%.3f,\"vblank_ms_per_frame\":%.3f}",
                  ms, vblank_ms);
    out += buf;
    return out;
}

// native_view stats' `emulated_gpu`: what the emulated GPU was sent, the
// calls per game frame (0 with no frames), and its command processor's CPU
// milliseconds per frame (-1 unknown); with renderer native, `present` false
// and the sync-only GPU's numbers
std::string EmulatedGpuJson(const NativeViewStats::EmulatedGpu& e) {
    const double frames = double(e.frames);
    auto per_frame = [&](uint64_t v) { return frames > 0 ? double(v) / frames : 0.0; };
    char buf[96];
    std::string out = "{\"mode\":";
    AppendJsonString(out, e.mode);
    out += ",\"skip_mode\":";
    out += e.skip_mode ? "true" : "false";
    out += ",\"skipping\":";
    out += e.skipping ? "true" : "false";
    out += ",\"fresh\":";
    out += e.fresh ? "true" : "false";
    out += ",\"frames\":" + std::to_string(e.frames);
    out += ",\"frames_skipped\":" + std::to_string(e.frames_skipped);
    for (const auto* list : {&e.emitted, &e.skipped}) {
        out += list == &e.emitted ? ",\"emitted_per_frame\":{" : ",\"skipped_per_frame\":{";
        for (size_t i = 0; i < list->size(); i++) {
            std::snprintf(buf, sizeof(buf), "%s\"%s\":%.1f", i ? "," : "",
                          (*list)[i].first.c_str(), per_frame((*list)[i].second));
            out += buf;
        }
        out += "}";
    }
    std::snprintf(buf, sizeof(buf), ",\"kept_per_frame\":{\"pass\":%.1f,\"point_tests\":%.1f}",
                  per_frame(e.kept_pass), per_frame(e.kept_point_tests));
    out += buf;
    out += ",\"passes_dropped\":" + std::to_string(e.passes_dropped);
    const double cp = e.cp_ms < 0 ? -1.0 : frames > 0 ? e.cp_ms / frames : 0.0;
    std::snprintf(buf, sizeof(buf), ",\"cp_ms_per_frame\":%.3f", cp);
    out += buf;
    if (!e.present) out += ",\"present\":false,\"sync\":" + SyncGpuJson(e.sync, frames);
    out += "}";
    return out;
}

// native_view stats' `by_kind`: each kind's frames, their times, and its
// totals per frame drawn (0 with none drawn)
std::string ByKindJson(const std::vector<NativeViewStats::Kind>& kinds) {
    char buf[96];
    std::string out = "{";
    for (size_t k = 0; k < kinds.size(); k++) {
        const NativeViewStats::Kind& kind = kinds[k];
        const double frames = double(kind.rendered);
        auto per_frame = [&](double v) { return frames > 0 ? v / frames : 0.0; };
        // "name":{...} after a comma (`first`: without), with a "total" first
        auto list = [&](const char* name, const std::vector<std::pair<std::string, double>>& l,
                        bool total, bool first = false) {
            out += first ? "\"" : ",\"";
            out += name;
            out += "\":{";
            double sum = 0;
            for (const auto& [n, v] : l) sum += v;
            if (total) {
                std::snprintf(buf, sizeof(buf), "\"total\":%.3f", per_frame(sum));
                out += buf;
            }
            for (size_t i = 0; i < l.size(); i++) {
                std::snprintf(buf, sizeof(buf), "%s\"%s\":%.3f", total || i ? "," : "",
                              l[i].first.c_str(), per_frame(l[i].second));
                out += buf;
            }
            out += "}";
        };
        if (k) out += ",";
        AppendJsonString(out, kind.name);
        out += ":{\"rendered\":" + std::to_string(kind.rendered);
        out += ",\"skipped_busy\":" + std::to_string(kind.skipped_busy);
        out += ",\"ms\":" + Distribution(kind.ms);
        out += ",\"wait_ms\":" + Distribution(kind.wait_ms);
        list("parts_ms_per_frame", kind.parts_ms, false);
        list("per_frame", kind.counts, false);
        out += ",\"plan_max_ms\":{";
        for (size_t i = 0; i < kind.plan_max_ms.size(); i++) {
            std::snprintf(buf, sizeof(buf), "%s\"%s\":%.2f", i ? "," : "",
                          kind.plan_max_ms[i].first.c_str(), kind.plan_max_ms[i].second);
            out += buf;
        }
        out += "}";
        out += ",\"plan_spikes\":" + std::to_string(kind.plan_spikes);
        out += ",\"plan_spikes_at_cut\":" + std::to_string(kind.plan_spikes_at_cut);
        out += ",\"capture\":{";
        list("ms_per_frame", kind.capture_ms, true, true);
        list("per_frame", kind.capture_counts, false);
        out += "},\"peak\":{";
        for (size_t i = 0; i < kind.peak.size(); i++) {
            std::snprintf(buf, sizeof(buf), "%s\"%s\":%.1f", i ? "," : "",
                          kind.peak[i].first.c_str(), kind.peak[i].second);
            out += buf;
        }
        out += "}";
        // native_gpu_timestamps: per frame timed, and only with some
        if (kind.gpu_timed) {
            const double timed = double(kind.gpu_timed);
            out += ",\"gpu_timed\":" + std::to_string(kind.gpu_timed);
            out += ",\"gpu_total_ms\":" + Distribution(kind.gpu_total_ms);
            out += ",\"gpu_ms\":{";
            for (size_t i = 0; i < kind.gpu_ms.size(); i++) {
                std::snprintf(buf, sizeof(buf), "%s\"%s\":%.3f", i ? "," : "",
                              kind.gpu_ms[i].first.c_str(), kind.gpu_ms[i].second / timed);
                out += buf;
            }
            std::snprintf(buf, sizeof(buf), "},\"gpu_marks_dropped\":%llu,\"gpu_bad_spans\":%llu",
                          static_cast<unsigned long long>(kind.gpu_marks_dropped),
                          static_cast<unsigned long long>(kind.gpu_bad_spans));
            out += buf;
        }
        out += "}";
    }
    out += "}";
    return out;
}

std::string NativeViewJson(const NativeViewStats& s) {
    std::string out = "\"stats\":{\"on\":";
    out += s.on ? "true" : "false";
    out += ",\"backend\":";
    AppendJsonString(out, s.backend);
    out += ",\"width\":" + std::to_string(s.width);
    out += ",\"height\":" + std::to_string(s.height);
    out += ",\"post\":";
    out += s.post ? "true" : "false";
    char buf[96];
    std::snprintf(buf, sizeof(buf), ",\"seconds\":%.1f,\"game_frames\":%llu,\"game_fps\":%.1f",
                  s.seconds, static_cast<unsigned long long>(s.game_frames),
                  s.seconds > 0 ? double(s.game_frames) / s.seconds : 0.0);
    out += buf;
    out += ",\"captured\":" + std::to_string(s.captured);
    out += ",\"rendered\":" + std::to_string(s.rendered);
    out += ",\"skipped_busy\":" + std::to_string(s.skipped_busy);
    out += ",\"worldless\":" + std::to_string(s.worldless);
    out += ",\"ms\":" + Distribution(s.frame_ms);
    out += ",\"wait_ms\":" + Distribution(s.wait_ms);
    out += ",\"in_flight_max\":" + std::to_string(s.in_flight_max);
    std::snprintf(buf, sizeof(buf), ",\"paused\":%s,\"paused_ms\":%.1f,\"paused_captures\":%llu",
                  s.paused ? "true" : "false", s.paused_ms,
                  static_cast<unsigned long long>(s.paused_captures));
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  ",\"camera\":{\"cuts\":%llu,\"vel_resets\":%llu,\"cuts_confirmed\":%llu}",
                  static_cast<unsigned long long>(s.camera_cuts),
                  static_cast<unsigned long long>(s.vel_resets),
                  static_cast<unsigned long long>(s.cuts_confirmed));
    out += buf;
    std::snprintf(buf, sizeof(buf),
                  ",\"rt_recording\":{\"on\":%s,\"passes\":%llu,\"recorded\":%llu,",
                  s.rt_on ? "true" : "false", static_cast<unsigned long long>(s.rt_passes),
                  static_cast<unsigned long long>(s.rt_recorded));
    out += buf;
    std::snprintf(buf, sizeof(buf), "\"draws\":%llu,\"ms\":%.2f}",
                  static_cast<unsigned long long>(s.rt_draws), s.rt_ms);
    out += buf;
    out += ",\"capture\":" + CaptureCostJson(s.capture);
    out += ",\"emulated_gpu\":" + EmulatedGpuJson(s.emulated_gpu);
    out += ",\"by_kind\":" + ByKindJson(s.by_kind);  // {} while it's off
    out += '}';
    return out;
}

// "1280x720"
std::optional<std::pair<uint32_t, uint32_t>> ParseSize(std::string_view text) {
    const size_t x = text.find('x');
    if (x == std::string_view::npos) return std::nullopt;
    auto w = ParseNumber<uint32_t>(text.substr(0, x));
    auto h = ParseNumber<uint32_t>(text.substr(x + 1));
    if (!w || !h || *w < 16 || *h < 16 || *w > 7680 || *h > 4320) return std::nullopt;
    return std::pair{*w, *h};
}

std::string NativeView(TestTarget& target, const std::vector<std::string_view>& args) {
    const std::string_view what = args.size() >= 2 ? args[1] : std::string_view{};
    if (what == "on" && args.size() <= 4) {
        std::pair<uint32_t, uint32_t> size{1280, 720};
        // nopost, last: without RB3's post-processing, to see what it costs
        size_t end = args.size();
        const bool post = !(end > 2 && args[end - 1] == "nopost");
        if (!post) end--;
        if (end == 4) return Error(target, "usage: native_view on [<width>x<height>] [nopost]");
        if (end == 3) {
            auto parsed = ParseSize(args[2]);
            if (!parsed) return Error(target, "a native view size is <width>x<height>, 16x16 up");
            size = *parsed;
        }
        if (std::string error = target.NativeViewOn(size.first, size.second, end == 3, post);
            !error.empty())
            return Error(target, error);
        return Ok(NativeViewJson(target.NativeView()));
    }
    if (what == "off" && args.size() == 2) {
        // what it measured, before off starts over
        const NativeViewStats stats = target.NativeView();
        target.NativeViewOff();
        return Ok(NativeViewJson(stats));
    }
    if (what == "stats" && args.size() == 2) return Ok(NativeViewJson(target.NativeView()));
    return Error(target, "usage: native_view on [<width>x<height>] [nopost]|off|stats");
}

std::string Set(TestTarget& target, std::string_view line,
                const std::vector<std::string_view>& args) {
    if (args.size() < 3) return Error(target, "usage: set <setting> <value>");
    // the value is the rest of the line, spaces and all
    const size_t value_start = static_cast<size_t>(args[2].data() - line.data());
    std::string_view value = line.substr(value_start);
    while (!value.empty() && (value.back() == ' ' || value.back() == '\r' || value.back() == '\t'))
        value.remove_suffix(1);
    if (std::string error = target.SetSetting(args[1], value); !error.empty())
        return Error(target, error);
    return Ok();
}

// cvar <name>: a setting's value and what set it, e.g. `cvar lang`
std::string Cvar(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: cvar <setting>");
    const auto setting = target.GetSetting(args[1]);
    if (!setting) return Error(target, "no setting " + std::string(args[1]));
    std::string out = "\"cvar\":{\"name\":";
    AppendJsonString(out, args[1]);
    out += ",\"value\":";
    AppendJsonString(out, setting->value);
    out += ",\"source\":";
    AppendJsonString(out, setting->source);
    out += '}';
    return Ok(out);
}

// folders: the game data, user data, cache and song folders the game runs with
std::string FoldersReply(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return Error(target, "usage: folders");
    const GameFolders folders = target.Folders();
    std::string out = "\"folders\":{\"game_data\":";
    AppendJsonString(out, folders.game_data);
    out += ",\"user_data\":";
    AppendJsonString(out, folders.user_data);
    out += ",\"cache\":";
    AppendJsonString(out, folders.cache);
    out += ",\"content\":[";
    for (size_t i = 0; i < folders.content.size(); i++) {
        if (i) out += ',';
        AppendJsonString(out, folders.content[i]);
    }
    out += "]}";
    return Ok(out);
}

// bind <name>: presses a key bind's key, e.g. `bind instrument_lab` for F6;
// the bind_ prefix is optional
std::string Bind(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: bind <name>, e.g. bind instrument_lab");
    std::string bind(args[1]);
    if (!bind.starts_with("bind_")) bind = "bind_" + bind;
    if (std::string error = target.PressBind(bind); !error.empty()) return Error(target, error);
    return Ok();
}

// type <text|{key}>...: types on the window's keyboard, e.g.
// `type the {space} who {enter}`
std::string Type(TestTarget& target, const std::vector<std::string_view>& args) {
    constexpr std::string_view kUsage = "usage: type <text|{key}>..., e.g. type abba {enter}";
    if (args.size() < 2) return Error(target, kUsage);
    std::vector<std::string> tokens;
    for (size_t i = 1; i < args.size(); i++) {
        const std::string_view token = args[i];
        if (token.starts_with('{')) {
            const std::string_view name = token.substr(1, token.size() - 1 - token.ends_with('}'));
            const auto& names = TypeKeyNames();
            if (!token.ends_with('}') || std::find(names.begin(), names.end(), name) == names.end()) {
                std::string error = "no key " + std::string(token) + " (";
                for (const std::string_view known : names) {
                    if (error.back() != '(') error += ' ';
                    error += "{" + std::string(known) + "}";
                }
                return Error(target, error + ")");
            }
        } else {
            for (const char c : token) {
                if (!TypeableCharacter(c)) {
                    return Error(target, std::string("can't type '") + c + "'; " + std::string(kUsage));
                }
            }
        }
        tokens.emplace_back(token);
    }
    if (std::string error = target.TypeKeys(tokens); !error.empty()) return Error(target, error);
    return Ok();
}

// liveless_invite <host[:port]> [force_flag]: player 1 accepts an invite to
// the Liveless game there, on RB3Enhanced's 9103 unless given
std::string LivelessInvite(TestTarget& target, const std::vector<std::string_view>& args) {
    constexpr std::string_view kUsage = "usage: liveless_invite <host[:port]> [force_flag]";
    if (args.size() < 2 || args.size() > 3) return Error(target, kUsage);
    const bool force_flag = args.size() == 3;
    if (force_flag && args[2] != "force_flag") return Error(target, kUsage);
    const auto game = online::ParseEndpoint(args[1], online::kGamePort);
    if (!game) return Error(target, std::string(args[1]) + " isn't an address (host or host:port)");
    if (std::string error = target.LivelessInvite(game->host, game->port, force_flag);
        !error.empty()) {
        return Error(target, error);
    }
    return Ok();
}

using StatusFields = std::vector<std::pair<std::string_view, std::string>>;

// A status command's checks, args[1] on: each field is the value
// (<field>=<value>), isn't it (<field>!=<value>; retry_in!=0: a retry is
// coming), or has the text in it (<field>~<text>, for errors, which have
// spaces). Empty when every check holds, else what's wrong: `verb`'s usage, a
// field it hasn't, or the field that isn't, named as `label`'s with the whole
// status (`json`) after it.
std::string CheckStatusFields(std::string_view verb, std::string_view label, const StatusFields& fields,
                              std::string_view json, const std::vector<std::string_view>& args) {
    for (size_t i = 1; i < args.size(); i++) {
        const size_t at = args[i].find_first_of("=~");
        const bool unequal =
            at != std::string_view::npos && at > 0 && args[i][at] == '=' && args[i][at - 1] == '!';
        const size_t name_end = unequal ? at - 1 : at;
        if (at == std::string_view::npos || name_end == 0) {
            return "usage: " + std::string(verb) + " [<field>=<value>|<field>!=<value>|<field>~<text>]...";
        }
        const std::string_view name = args[i].substr(0, name_end), want = args[i].substr(at + 1);
        const bool contains = args[i][at] == '~';
        const auto field = std::find_if(fields.begin(), fields.end(),
                                        [&](const auto& f) { return f.first == name; });
        if (field == fields.end()) return std::string(verb) + " has no field " + std::string(name);
        const bool holds = contains  ? field->second.find(want) != std::string::npos
                           : unequal ? field->second != want
                                     : field->second == want;
        if (!holds) {
            const char* how = contains ? "containing " : unequal ? "other than " : "";
            return std::string(label) + " " + std::string(name) + " is \"" + field->second + "\", not " +
                   how + "\"" + std::string(want) + "\"; {" + std::string(json) + "}";
        }
    }
    return {};
}

// Liveless Rooms' status, each field as text: addresses dotted, empty for none
StatusFields RoomsFields(const rooms::Status& s) {
    auto ip = [](uint32_t address) { return address ? rooms::Ipv4Text(address) : std::string(); };
    return {
        {"state", std::string(rooms::StateName(s.state))},
        {"server", s.server},
        {"code", s.code},
        {"public_ip", ip(s.public_ipv4)},
        {"advertised_ip", ip(s.advertised_ipv4)},
        {"error", s.error},
        {"last_join_user", s.last_join_user},
        {"last_join_ip", ip(s.last_join_ipv4)},
        {"game_socket", s.game_socket_seen ? "true" : "false"},
        {"retry_in", std::to_string(s.retry_in_s)},
        {"attempt", std::to_string(s.attempt)},
    };
}

std::string RoomsJson(const rooms::Status& s) {
    std::string out = "\"rooms\":{";
    bool first = true;
    for (const auto& [name, value] : RoomsFields(s)) {
        if (!first) out += ',';
        first = false;
        AppendJsonString(out, name);
        out += ':';
        // a boolean and numbers, bare
        if (name == "game_socket" || name == "retry_in" || name == "attempt") {
            out += value;
        } else {
            AppendJsonString(out, value);
        }
    }
    out += '}';
    return out;
}

// rooms_status [<field>=<value>|<field>!=<value>|<field>~<text>]...: Liveless
// Rooms' status; with checks (CheckStatusFields'), a failure unless each holds
std::string RoomsStatus(TestTarget& target, const std::vector<std::string_view>& args) {
    const rooms::Status status = target.RoomsStatus();
    const std::string json = RoomsJson(status);
    if (std::string error = CheckStatusFields("rooms_status", "rooms", RoomsFields(status), json, args);
        !error.empty()) {
        return Error(target, error);
    }
    return Ok(json);
}

// rooms_join <code>: asks the Rooms server for the game with that code
std::string RoomsJoin(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: rooms_join <code>");
    std::string code(args[1]);
    if (std::string error = rooms::NormalizeCode(code); !error.empty()) return Error(target, error);
    if (std::string error = target.RoomsJoin(code); !error.empty()) return Error(target, error);
    std::string fields = "\"code\":";
    AppendJsonString(fields, code);
    return Ok(fields);
}

// rooms_connect: connects to the Rooms server again
std::string RoomsConnect(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 1) return Error(target, "usage: rooms_connect");
    if (std::string error = target.RoomsConnect(); !error.empty()) return Error(target, error);
    return Ok();
}

// Liveless' port mapping, each field as text: the address dotted, empty for
// none; port and lease_s are numbers in the JSON
StatusFields PortMappingFields(const port_mapping::Status& s) {
    return {
        {"state", std::string(port_mapping::StateName(s.state))},
        {"method", std::string(port_mapping::MethodName(s.method))},
        {"external_ip", s.external_ipv4 ? rooms::Ipv4Text(s.external_ipv4) : std::string()},
        {"port", std::to_string(s.port)},
        {"lease_s", std::to_string(s.lease_s)},
        {"error", s.error},
    };
}

std::string PortMappingJson(const port_mapping::Status& s) {
    std::string out = "\"port_mapping\":{";
    bool first = true;
    for (const auto& [name, value] : PortMappingFields(s)) {
        if (!first) out += ',';
        first = false;
        AppendJsonString(out, name);
        out += ':';
        if (name == "port" || name == "lease_s") {
            out += value;
        } else {
            AppendJsonString(out, value);
        }
    }
    out += '}';
    return out;
}

// port_mapping_status [<field>=<value>|<field>!=<value>|<field>~<text>]...:
// the port mapping's status; with checks (CheckStatusFields'), a failure
// unless each holds
std::string PortMappingStatus(TestTarget& target, const std::vector<std::string_view>& args) {
    const port_mapping::Status status = target.PortMappingStatus();
    const std::string json = PortMappingJson(status);
    if (std::string error = CheckStatusFields("port_mapping_status", "port_mapping",
                                              PortMappingFields(status), json, args);
        !error.empty()) {
        return Error(target, error);
    }
    return Ok(json);
}

}

std::variant<Condition, std::string> ParseCondition(std::string_view text) {
    Condition c;
    if (text == "in_game") {
        c.kind = Condition::Kind::kInGame;
    } else if (text == "menus") {
        c.kind = Condition::Kind::kMenus;
    } else if (text == "joined") {
        c.kind = Condition::Kind::kJoined;
    } else if (text.starts_with("screen=")) {
        c.kind = Condition::Kind::kScreen;
        c.text = text.substr(7);
    } else if (text.starts_with("screen~")) {
        c.kind = Condition::Kind::kScreenContains;
        c.text = text.substr(7);
    } else if (text.starts_with("song=")) {
        c.kind = Condition::Kind::kSong;
        c.text = text.substr(5);
    } else if (text.starts_with("frames=")) {
        auto n = ParseNumber<uint64_t>(text.substr(7));
        if (!n) return "frames= takes a number";
        c.kind = Condition::Kind::kFrames;
        c.frames = *n;
    } else if (text.starts_with("score>=")) {
        auto n = ParseNumber<int64_t>(text.substr(7));
        if (!n) return "score>= takes a number";
        c.kind = Condition::Kind::kScore;
        c.score = *n;
    } else if (text.starts_with("mic=")) {
        auto n = ParseNumber<int>(text.substr(4));
        if (!n || *n < 1 || *n > kMicSlots) return "mic= takes a mic slot, 1 to 4";
        c.kind = Condition::Kind::kMic;
        c.mic = *n;
    } else if (text.starts_with("rooms=")) {
        c.kind = Condition::Kind::kRooms;
        c.text = text.substr(6);
        bool known = false;
        for (auto state : {rooms::State::kOff, rooms::State::kConnecting, rooms::State::kConnected,
                           rooms::State::kLoggedIn, rooms::State::kDisconnected, rooms::State::kFailed}) {
            known |= c.text == rooms::StateName(state);
        }
        if (!known) return "rooms= takes off, connecting, connected, logged_in, disconnected or failed";
    } else if (text.starts_with("port_mapping=")) {
        c.kind = Condition::Kind::kPortMapping;
        c.text = text.substr(13);
        bool known = false;
        for (auto state : {port_mapping::State::kOff, port_mapping::State::kSearching,
                           port_mapping::State::kMapped, port_mapping::State::kFailed}) {
            known |= c.text == port_mapping::StateName(state);
        }
        if (!known) return "port_mapping= takes off, searching, mapped or failed";
    } else {
        return "no condition " + std::string(text) +
               " (screen=, screen~, in_game, menus, song=, frames=, score>=, mic=, rooms=, "
               "port_mapping=, joined)";
    }
    if ((c.kind == Condition::Kind::kScreen || c.kind == Condition::Kind::kScreenContains ||
         c.kind == Condition::Kind::kSong) &&
        c.text.empty()) {
        return std::string(text) + " needs a name";
    }
    return c;
}

bool ConditionHolds(const Condition& condition, const GameStateSnapshot& state,
                    const GameStateSnapshot& start) {
    switch (condition.kind) {
    case Condition::Kind::kScreen: return state.screen == condition.text;
    case Condition::Kind::kScreenContains:
        return state.screen.find(condition.text) != std::string::npos;
    case Condition::Kind::kInGame: return state.in_game;
    case Condition::Kind::kMenus: return !state.in_game;
    case Condition::Kind::kSong: return state.song_shortname == condition.text;
    case Condition::Kind::kFrames: return state.frame - start.frame >= condition.frames;
    case Condition::Kind::kScore: return state.score >= condition.score;
    case Condition::Kind::kMic: {
        // connected, and handed the game audio since the wait began
        const size_t slot = static_cast<size_t>(condition.mic - 1);
        if (slot >= state.mics.size() || !state.mics[slot].connected) return false;
        const uint64_t fed_before = slot < start.mics.size() ? start.mics[slot].bytes_fed : 0;
        return state.mics[slot].bytes_fed > fed_before;
    }
    case Condition::Kind::kRooms: return state.rooms_state == condition.text;
    case Condition::Kind::kPortMapping: return state.port_mapping_state == condition.text;
    case Condition::Kind::kJoined: return state.joined;
    }
    return false;
}

const std::vector<std::string_view>& TypeKeyNames() {
    static const std::vector<std::string_view> names = {
        "enter", "back", "tab", "esc", "space", "left", "right", "up", "down", "delete"};
    return names;
}

bool TypeableCharacter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           std::string_view("-.,'/`").find(c) != std::string_view::npos;
}

std::string RunCommand(std::string_view line, TestTarget& target) {
    std::vector<std::string_view> args = Words(line);
    if (args.empty()) return Error(target, "empty command");

    // pN: the player a controller command is for, player 1 without one
    std::optional<int> prefix;
    const bool verb_first = args[0] == "press" || args[0] == "pad";
    if (!verb_first && args[0].size() <= 3 && args[0].starts_with("p")) {
        auto player = ParseNumber<int>(args[0].substr(1));
        if (!player || *player < 1 || *player > kPlayerCount) {
            return Error(target, "no player " + std::string(args[0]) + " (p1 to p4)");
        }
        if (args.size() == 1) return Error(target, std::string(args[0]) + " needs a command");
        prefix = *player;
        args.erase(args.begin());
    }
    const std::string_view verb = args[0];
    const int player = prefix.value_or(1);

    if (verb == "pad") return Pad(target, prefix, args);
    if (verb == "instrument") return Instrument(target, player, args);
    if (verb == "unplug") return Unplug(target, player, args);
    if (verb == "press" || verb == "hit" || verb == "hold" || verb == "release" ||
        verb == "axis") {
        auto kind = target.Kind(player);
        if (!kind) {
            const std::string p = std::to_string(player);
            return Error(target, "player " + p + " has no virtual instrument (p" + p +
                                     " instrument <kind> plugs one in)");
        }
        const Controller c{player, *kind};
        if (verb == "press") return Press(target, c, args);
        if (verb == "hit") return Hit(target, c, args);
        if (verb == "hold") return Hold(target, c, args, true);
        if (verb == "release") return Hold(target, c, args, false);
        return Axis(target, c, args);
    }
    if (prefix) {
        return Error(target, std::string(verb) + " isn't for one player; leave out the p" +
                                 std::to_string(player));
    }

    if (verb == "state") return OkWithState(target, target.State());
    if (verb == "wait") return Wait(target, args, kWaitTimeout);
    if (verb == "expect") return Wait(target, args, kExpectTimeout);
    if (verb == "sleep") return SleepFor(target, args);
    if (verb == "screenshot") return Screenshot(target, args);
    if (verb == "capture") return Capture(target, args);
    if (verb == "set") return Set(target, line, args);
    if (verb == "cvar") return Cvar(target, args);
    if (verb == "folders") return FoldersReply(target, args);
    if (verb == "bind") return Bind(target, args);
    if (verb == "type") return Type(target, args);
    if (verb == "liveless_invite") return LivelessInvite(target, args);
    if (verb == "rooms_status") return RoomsStatus(target, args);
    if (verb == "rooms_join") return RoomsJoin(target, args);
    if (verb == "rooms_connect") return RoomsConnect(target, args);
    if (verb == "port_mapping_status") return PortMappingStatus(target, args);
    if (verb == "native_view") return NativeView(target, args);
    if (verb == "present_stats") return PresentStatsCommand(target, args);
    if (verb == "quit") {
        target.Quit();
        return Ok();
    }
    return Error(target, "no command " + std::string(verb));
}

void ReleaseAllPlayers(TestTarget& target) {
    for (int player = 1; player <= kPlayerCount; player++) {
        if (target.Kind(player)) target.SetHeld(player, {});
    }
}

}
