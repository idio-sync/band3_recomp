#include "test_commands.h"
#include <charconv>
#include <cstdio>
#include <optional>
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
// about a frame
constexpr std::chrono::milliseconds kWaitPoll = 16ms;
constexpr uint8_t kDefaultVelocity = 100;

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

std::string StateJson(const GameStateSnapshot& s, input::InstrumentKind kind) {
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
    out += ",\"instrument\":";
    AppendJsonString(out, InstrumentName(kind));
    out += '}';
    return out;
}

std::string Error(TestTarget& target, std::string_view message) {
    std::string out = "{\"ok\":false,\"error\":";
    AppendJsonString(out, message);
    out += ",\"state\":" + StateJson(target.State(), target.Kind()) + "}";
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
    return Ok("\"state\":" + StateJson(state, target.Kind()));
}

// applies every name in `list` to `in` at `value`; an error, or empty
std::string ApplyInputs(TestTarget& target, InstrumentInputs& in, std::string_view list,
                        uint8_t value) {
    for (const std::string& name : SplitInputs(list)) {
        if (name.empty()) return "empty input name in " + std::string(list);
        std::string error = SetInput(target.Kind(), in, name, value);
        if (!error.empty()) return error;
    }
    return {};
}

std::string Press(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() < 2 || args.size() > 3) return Error(target, "usage: press <inputs> [ms]");
    std::chrono::milliseconds length = kPressLength;
    if (args.size() == 3) {
        auto ms = ParseNumber<int64_t>(args[2]);
        if (!ms || *ms < 1 || *ms > 10000) return Error(target, "press length is 1 to 10000 ms");
        length = std::chrono::milliseconds(*ms);
    }
    // checked on a copy, so a bad name presses nothing
    InstrumentInputs check;
    if (std::string error = ApplyInputs(target, check, args[1], kDefaultVelocity); !error.empty())
        return Error(target, error);

    const input::InstrumentKind kind = target.Kind();
    const std::string list(args[1]);
    target.Pulse([kind, list](InstrumentInputs& in) {
        for (const std::string& name : SplitInputs(list)) SetInput(kind, in, name, kDefaultVelocity);
    }, length);
    target.Sleep(length + kReleaseGap);
    return Ok();
}

std::string Hit(TestTarget& target, const std::vector<std::string_view>& args) {
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
    const input::InstrumentKind kind = target.Kind();
    const std::string name(args[1]);
    if (std::string error = SetInput(kind, check, name, static_cast<uint8_t>(velocity),
                                     static_cast<uint8_t>(fret));
        !error.empty())
        return Error(target, error);

    target.Pulse([kind, name, velocity, fret](InstrumentInputs& in) {
        SetInput(kind, in, name, static_cast<uint8_t>(velocity), static_cast<uint8_t>(fret));
    }, kPressLength);
    target.Sleep(kPressLength + kReleaseGap);
    return Ok();
}

std::string Hold(TestTarget& target, const std::vector<std::string_view>& args, bool down) {
    if (args.size() != 2) {
        return Error(target, down ? "usage: hold <inputs>" : "usage: release <inputs>|all");
    }
    if (!down && args[1] == "all") {
        target.SetHeld({});
        return Ok();
    }
    InstrumentInputs held = target.Held();
    if (std::string error = ApplyInputs(target, held, args[1], down ? kDefaultVelocity : 0);
        !error.empty())
        return Error(target, error);
    target.SetHeld(held);
    return Ok();
}

std::string Axis(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 3) return Error(target, "usage: axis whammy|tilt <0..1>");
    auto value = ParseNumber<float>(args[2]);
    if (!value) return Error(target, "bad axis value " + std::string(args[2]));
    InstrumentInputs held = target.Held();
    if (std::string error = SetAxis(target.Kind(), held, args[1], *value); !error.empty())
        return Error(target, error);
    target.SetHeld(held);
    return Ok();
}

std::string Instrument(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() != 2) return Error(target, "usage: instrument guitar|drums|keys|mustang|squier");
    auto kind = ParseInstrumentName(args[1]);
    if (!kind) return Error(target, "no instrument " + std::string(args[1]));
    if (*kind != target.Kind()) {
        target.SetHeld({});
        target.SetKind(*kind);
        target.Sleep(kReplugWait);
    }
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
    GameStateSnapshot state = target.State();
    const uint64_t start_frame = state.frame;
    while (!ConditionHolds(condition, state, start_frame)) {
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

bool IsFileName(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

std::string Screenshot(TestTarget& target, const std::vector<std::string_view>& args) {
    if (args.size() > 2) return Error(target, "usage: screenshot [name]");
    std::string name;
    if (args.size() == 2) {
        if (!IsFileName(args[1])) {
            return Error(target, "a screenshot name is up to 64 letters, digits, _ and -");
        }
        name = args[1];
    }
    ScreenshotInfo info;
    if (std::string error = target.Screenshot(name, info); !error.empty())
        return Error(target, error);
    std::string fields = "\"path\":";
    AppendJsonString(fields, info.path);
    fields += ",\"width\":" + std::to_string(info.width);
    fields += ",\"height\":" + std::to_string(info.height);
    return Ok(fields);
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
    } else {
        return "no condition " + std::string(text) +
               " (screen=, screen~, in_game, menus, song=, frames=)";
    }
    if ((c.kind == Condition::Kind::kScreen || c.kind == Condition::Kind::kScreenContains ||
         c.kind == Condition::Kind::kSong) &&
        c.text.empty()) {
        return std::string(text) + " needs a name";
    }
    return c;
}

bool ConditionHolds(const Condition& condition, const GameStateSnapshot& state,
                    uint64_t start_frame) {
    switch (condition.kind) {
    case Condition::Kind::kScreen: return state.screen == condition.text;
    case Condition::Kind::kScreenContains:
        return state.screen.find(condition.text) != std::string::npos;
    case Condition::Kind::kInGame: return state.in_game;
    case Condition::Kind::kMenus: return !state.in_game;
    case Condition::Kind::kSong: return state.song_shortname == condition.text;
    case Condition::Kind::kFrames: return state.frame - start_frame >= condition.frames;
    }
    return false;
}

std::string RunCommand(std::string_view line, TestTarget& target) {
    const std::vector<std::string_view> args = Words(line);
    if (args.empty()) return Error(target, "empty command");
    const std::string_view verb = args[0];

    if (verb == "state") return OkWithState(target, target.State());
    if (verb == "press") return Press(target, args);
    if (verb == "hit") return Hit(target, args);
    if (verb == "hold") return Hold(target, args, true);
    if (verb == "release") return Hold(target, args, false);
    if (verb == "axis") return Axis(target, args);
    if (verb == "instrument") return Instrument(target, args);
    if (verb == "wait") return Wait(target, args, kWaitTimeout);
    if (verb == "expect") return Wait(target, args, kExpectTimeout);
    if (verb == "screenshot") return Screenshot(target, args);
    if (verb == "set") return Set(target, line, args);
    if (verb == "quit") {
        target.Quit();
        return Ok();
    }
    return Error(target, "no command " + std::string(verb));
}

}
