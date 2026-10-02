#include "test_commands.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <optional>
#include <utility>
#include <vector>
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

std::string Press(TestTarget& target, const Controller& c,
                  const std::vector<std::string_view>& args) {
    if (args.size() < 2 || args.size() > 3) return Error(target, "usage: press <inputs> [ms]");
    std::chrono::milliseconds length = kPressLength;
    if (args.size() == 3) {
        auto ms = ParseNumber<int64_t>(args[2]);
        if (!ms || *ms < 1 || *ms > 10000) return Error(target, "press length is 1 to 10000 ms");
        length = std::chrono::milliseconds(*ms);
    }
    // checked on a copy, so a bad name presses nothing
    InstrumentInputs check;
    if (std::string error = ApplyInputs(c, check, args[1], kDefaultVelocity); !error.empty())
        return Error(target, error);

    const input::InstrumentKind kind = c.kind;
    const std::string list(args[1]);
    target.Pulse(c.player, [kind, list](InstrumentInputs& in) {
        for (const std::string& name : SplitInputs(list)) SetInput(kind, in, name, kDefaultVelocity);
    }, length);
    target.Sleep(length + kReleaseGap);
    return Ok();
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
    std::snprintf(buf, sizeof(buf),
                  ",\"rt_recording\":{\"on\":%s,\"passes\":%llu,\"recorded\":%llu,",
                  s.rt_on ? "true" : "false", static_cast<unsigned long long>(s.rt_passes),
                  static_cast<unsigned long long>(s.rt_recorded));
    out += buf;
    std::snprintf(buf, sizeof(buf), "\"draws\":%llu,\"ms\":%.2f}",
                  static_cast<unsigned long long>(s.rt_draws), s.rt_ms);
    out += buf;
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

// bind <name>: presses a key bind's key, e.g. `bind instrument_lab` for F6;
// the bind_ prefix is optional
std::string Bind(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: bind <name>, e.g. bind instrument_lab");
    std::string bind(args[1]);
    if (!bind.starts_with("bind_")) bind = "bind_" + bind;
    if (std::string error = target.PressBind(bind); !error.empty()) return Error(target, error);
    return Ok();
}

}

std::variant<Condition, std::string> ParseCondition(std::string_view text) {
    Condition c;
    if (text == "in_game") {
        c.kind = Condition::Kind::kInGame;
    } else if (text == "menus") {
        c.kind = Condition::Kind::kMenus;
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
    } else {
        return "no condition " + std::string(text) +
               " (screen=, screen~, in_game, menus, song=, frames=, score>=, mic=)";
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
    }
    return false;
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
    if (verb == "bind") return Bind(target, args);
    if (verb == "native_view") return NativeView(target, args);
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
