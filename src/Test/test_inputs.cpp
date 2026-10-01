#include "test_inputs.h"
#include <charconv>
#include <string>

namespace band3::test {

using input::InstrumentInputs;
using input::InstrumentKind;
using input::NavInputs;

namespace {

bool SetNav(NavInputs& nav, std::string_view name, bool down) {
    bool* button = nullptr;
    if (name == "a") button = &nav.a;
    else if (name == "b") button = &nav.b;
    else if (name == "x") button = &nav.x;
    else if (name == "y") button = &nav.y;
    else if (name == "start") button = &nav.start;
    else if (name == "back") button = &nav.back;
    else if (name == "up") button = &nav.dpad_up;
    else if (name == "down") button = &nav.dpad_down;
    else if (name == "left") button = &nav.dpad_left;
    else if (name == "right") button = &nav.dpad_right;
    if (!button) return false;
    *button = down;
    return true;
}

// the 5-fret colors, in Fret order
constexpr std::string_view kFretNames[input::kFretCount] = {"green", "red", "yellow", "blue",
                                                            "orange"};

bool SetFret(std::array<bool, input::kFretCount>& frets, std::string_view name, bool down) {
    for (int f = 0; f < input::kFretCount; f++) {
        if (name == kFretNames[f]) {
            frets[f] = down;
            return true;
        }
    }
    return false;
}

bool SetGuitar(input::GuitarInputs& g, std::string_view name, bool down) {
    if (SetNav(g.nav, name, down) || SetFret(g.frets, name, down)) return true;
    if (name == "strum_up") g.strum_up = down;
    else if (name == "strum_down") g.strum_down = down;
    else if (name == "solo") g.solo = down;
    else return false;
    return true;
}

bool SetDrums(input::DrumInputs& d, std::string_view name, uint8_t value) {
    if (SetNav(d.nav, name, value != 0)) return true;
    constexpr std::string_view kPads[input::kPadCount] = {"red_pad", "yellow_pad", "blue_pad",
                                                          "green_pad"};
    constexpr std::string_view kCymbals[input::kCymbalCount] = {"yellow_cym", "blue_cym",
                                                                "green_cym"};
    for (int p = 0; p < input::kPadCount; p++) {
        if (name == kPads[p]) {
            d.pads[p] = value;
            return true;
        }
    }
    for (int c = 0; c < input::kCymbalCount; c++) {
        if (name == kCymbals[c]) {
            d.cymbals[c] = value;
            return true;
        }
    }
    if (name == "kick") d.kick1 = value != 0;
    else if (name == "kick2") d.kick2 = value != 0;
    else return false;
    return true;
}

bool SetKeys(input::KeysInputs& k, std::string_view name, uint8_t value) {
    if (SetNav(k.nav, name, value != 0)) return true;
    if (name == "overdrive") {
        k.overdrive = value != 0;
        return true;
    }
    if (!name.starts_with("key") || name.size() == 3) return false;
    int key = -1;
    const auto digits = name.substr(3);
    const auto [end, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), key);
    if (ec != std::errc() || end != digits.data() + digits.size()) return false;
    if (key < 0 || key >= input::kKeyCount) return false;
    k.keys[key] = value;
    return true;
}

// not "a" and "b", which are the buttons every instrument has
constexpr std::string_view kStringNames[input::kStringCount] = {"low_e", "a_str", "d_str",
                                                                "g_str", "b_str", "high_e"};

int StringIndex(std::string_view name) {
    for (int s = 0; s < input::kStringCount; s++) {
        if (name == kStringNames[s]) return s;
    }
    return -1;
}

}

std::string SetInput(InstrumentKind kind, InstrumentInputs& in, std::string_view name,
                     uint8_t value, uint8_t fret) {
    const bool pro_guitar = kind == InstrumentKind::kProGuitarMustang ||
                            kind == InstrumentKind::kProGuitarSquier;
    const int string = pro_guitar ? StringIndex(name) : -1;
    if (fret != 0 && string < 0) {
        return "a fret is only for pro guitar strings, not " + std::string(name);
    }

    bool known = false;
    switch (kind) {
    case InstrumentKind::kGuitar: known = SetGuitar(in.guitar, name, value != 0); break;
    case InstrumentKind::kDrums: known = SetDrums(in.drums, name, value); break;
    case InstrumentKind::kKeys: known = SetKeys(in.keys, name, value); break;
    case InstrumentKind::kProGuitarMustang:
    case InstrumentKind::kProGuitarSquier: {
        auto& p = in.pro_guitar;
        if (string >= 0) {
            if (fret > input::kMaxProFret) {
                return "fret " + std::to_string(fret) + " is past the last (" +
                       std::to_string(input::kMaxProFret) + ")";
            }
            p.frets[string] = value != 0 ? fret : 0;
            p.velocities[string] = value;
            known = true;
        } else if (name == "solo") {
            p.solo = value != 0;
            known = true;
        } else {
            known = SetNav(p.nav, name, value != 0) || SetFret(p.colors, name, value != 0);
        }
        break;
    }
    }
    if (!known) return std::string(InstrumentName(kind)) + " has no input " + std::string(name);
    return {};
}

std::string SetAxis(InstrumentKind kind, InstrumentInputs& in, std::string_view name,
                    float value) {
    if (kind != InstrumentKind::kGuitar) {
        return std::string(InstrumentName(kind)) + " has no axis " + std::string(name);
    }
    if (!(value >= 0.0f && value <= 1.0f)) return "an axis goes from 0 to 1";
    if (name == "whammy") in.guitar.whammy = value;
    else if (name == "tilt") in.guitar.tilt = value;
    else return "guitar has no axis " + std::string(name);
    return {};
}

std::vector<std::string> SplitInputs(std::string_view list) {
    std::vector<std::string> names;
    size_t start = 0;
    while (true) {
        const size_t plus = list.find('+', start);
        names.emplace_back(list.substr(start, plus - start));
        if (plus == std::string_view::npos) break;
        start = plus + 1;
    }
    return names;
}

std::optional<InstrumentKind> ParseInstrumentName(std::string_view name) {
    for (InstrumentKind kind : input::kInstrumentKinds) {
        if (name == InstrumentName(kind)) return kind;
    }
    return std::nullopt;
}

const char* InstrumentName(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return "guitar";
    case InstrumentKind::kDrums: return "drums";
    case InstrumentKind::kKeys: return "keys";
    case InstrumentKind::kProGuitarMustang: return "mustang";
    case InstrumentKind::kProGuitarSquier: return "squier";
    }
    return "guitar";
}

}
