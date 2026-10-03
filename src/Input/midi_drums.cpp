#include "midi_drums.h"
#include <algorithm>
#include <charconv>

namespace band3::input::midi_drums {

namespace {

struct PartEntry {
    Part part;
    const char* name;
};

constexpr PartEntry kParts[] = {
    {Part::kKick, "Kick"},         {Part::kHihatPedal, "HihatPedal"},
    {Part::kSnare, "Snare"},       {Part::kSnareRim, "SnareRim"},
    {Part::kHiTom, "HiTom"},       {Part::kLowTom, "LowTom"},
    {Part::kFloorTom, "FloorTom"}, {Part::kHihat, "Hihat"},
    {Part::kRide, "Ride"},         {Part::kCrash, "Crash"},
};

// menu buttons from the kit, as in RPCS3
constexpr const char* kStartCombo = "start";
constexpr const char* kSelectCombo = "select";
constexpr const char* kHoldKickCombo = "hold kick";

struct Combo {
    const char* name;
    std::array<Part, 4> parts;
};

constexpr Combo kCombos[] = {
    {kStartCombo, {Part::kHihatPedal, Part::kHihatPedal, Part::kHihatPedal, Part::kSnare}},
    {kSelectCombo, {Part::kHihatPedal, Part::kHihatPedal, Part::kHihatPedal, Part::kSnareRim}},
    {kHoldKickCombo, {Part::kHihatPedal, Part::kHihatPedal, Part::kHihatPedal, Part::kKick}},
};

std::string_view Trim(std::string_view s) {
    while (!s.empty() && s.front() == ' ') s.remove_prefix(1);
    while (!s.empty() && s.back() == ' ') s.remove_suffix(1);
    return s;
}

void Apply(DrumInputs& in, Part part, uint8_t velocity) {
    auto hit = [velocity](uint8_t& slot) { slot = std::max(slot, velocity); };
    switch (part) {
    case Part::kKick: in.kick1 = true; break;
    case Part::kHihatPedal: in.kick2 = true; break;
    case Part::kSnare:
    case Part::kSnareRim: hit(in.pads[kRedPad]); break;
    case Part::kHiTom: hit(in.pads[kYellowPad]); break;
    case Part::kLowTom: hit(in.pads[kBluePad]); break;
    case Part::kFloorTom: hit(in.pads[kGreenPad]); break;
    case Part::kHihat: hit(in.cymbals[kYellowCymbal]); break;
    case Part::kRide: hit(in.cymbals[kBlueCymbal]); break;
    case Part::kCrash: hit(in.cymbals[kGreenCymbal]); break;
    case Part::kNone: break;
    }
}

}

const char* PartName(Part part) {
    for (const auto& entry : kParts) {
        if (entry.part == part) return entry.name;
    }
    return "None";
}

std::optional<Part> ParsePart(std::string_view name) {
    for (const auto& entry : kParts) {
        if (name == entry.name) return entry.part;
    }
    // RPCS3's name for an open hi-hat, which is a hi-hat hit here
    if (name == "HihatWithPedalUp") return Part::kHihat;
    return std::nullopt;
}

NoteMap DefaultNoteMap() {
    NoteMap map{};
    auto set = [&map](Part part, std::initializer_list<uint8_t> notes) {
        for (uint8_t note : notes) map[note] = part;
    };
    set(Part::kKick, {33, 35, 36});
    // 23 is a partly closed hi-hat pedal on Alesis kits
    set(Part::kHihatPedal, {44, 23});
    set(Part::kSnare, {38, 31, 34, 37, 39});
    // the MIDI Pro Adapter counts a rim shot as a snare hit
    set(Part::kSnareRim, {40});
    set(Part::kHiTom, {48, 50});
    set(Part::kLowTom, {45, 47});
    set(Part::kFloorTom, {41, 43});
    // 46 is an open hi-hat
    set(Part::kHihat, {22, 26, 42, 54, 46});
    set(Part::kRide, {51, 53, 56, 59});
    set(Part::kCrash, {49, 52, 55, 57});
    return map;
}

std::vector<std::string> ApplyOverrides(NoteMap& map, std::string_view overrides) {
    std::vector<std::string> bad;
    while (!overrides.empty()) {
        const size_t comma = overrides.find(',');
        const std::string_view entry = Trim(overrides.substr(0, comma));
        overrides.remove_prefix(comma == std::string_view::npos ? overrides.size() : comma + 1);
        if (entry.empty()) continue;

        const size_t equals = entry.find('=');
        unsigned note = 0;
        std::optional<Part> part;
        if (equals != std::string_view::npos) {
            const std::string_view number = Trim(entry.substr(0, equals));
            auto [end, ec] = std::from_chars(number.data(), number.data() + number.size(), note);
            if (ec == std::errc() && end == number.data() + number.size() && note < map.size()) {
                const std::string_view name = Trim(entry.substr(equals + 1));
                part = name == "None" ? Part::kNone : ParsePart(name);
            }
        }
        if (!part) {
            bad.emplace_back(entry);
            continue;
        }
        map[note] = *part;
    }
    return bad;
}

bool Kit::Pulse::IsCymbal() const {
    return part == Part::kHihat || part == Part::kRide || part == Part::kCrash;
}

Kit::Kit(const NoteMap& notes, const Settings& settings) : notes_(notes), settings_(settings) {}

void Kit::SetSettings(const Settings& settings) {
    settings_ = settings;
    if (!settings_.combos) combo_.clear();
}

const char* Kit::TrackCombo(Part part, Clock::time_point now) {
    if (!combo_.empty() && now >= combo_expiry_) combo_.clear();

    auto continues = [&] {
        const size_t i = combo_.size();
        return std::ranges::any_of(kCombos, [&](const Combo& c) {
            return i < c.parts.size() && c.parts[i] == part;
        });
    };
    // a note that breaks a combo in progress may still start a new one
    if (!continues()) {
        combo_.clear();
        if (!continues()) return nullptr;
    }
    if (combo_.empty()) combo_expiry_ = now + settings_.combo_window;
    combo_.push_back(part);

    for (const auto& c : kCombos) {
        if (std::ranges::equal(combo_, c.parts)) {
            combo_.clear();
            return c.name;
        }
    }
    return nullptr;
}

std::optional<Hit> Kit::Receive(std::span<const uint8_t> message, Clock::time_point now) {
    // note-on on any channel; velocity 0 is a note-off
    if (message.size() < 3 || (message[0] & 0xF0) != 0x90) return std::nullopt;
    Hit hit;
    hit.note = message[1] & 0x7F;
    hit.velocity = message[2] & 0x7F;
    if (hit.velocity == 0) return std::nullopt;
    hit.part = notes_[hit.note];
    if (hit.part == Part::kNone || hit.velocity < settings_.min_velocity) return hit;

    if (settings_.combos) {
        hit.combo = TrackCombo(hit.part, now);
        if (hit.combo == kHoldKickCombo) {
            hold_kick_ = !hold_kick_;
            return hit;
        }
        if (hit.combo) {
            Pulse button;
            button.began = now;
            button.start = hit.combo == kStartCombo;
            button.select = hit.combo == kSelectCombo;
            pulses_.push_back(button);
            return hit;
        }
    }

    // a held kick opens RB3's song category menu; snare or floor tom closes it
    if (hold_kick_ && (hit.part == Part::kSnare || hit.part == Part::kFloorTom)) {
        hold_kick_ = false;
        return hit;
    }

    pulses_.push_back({now, hit.part, hit.velocity});
    return hit;
}

DrumInputs Kit::State(Clock::time_point now) {
    const auto pulse = settings_.pulse;
    std::erase_if(pulses_, [now, pulse](const Pulse& p) {
        return !p.waiting && now >= p.began + pulse;
    });

    DrumInputs in;
    bool cymbal = false;
    for (auto& p : pulses_) {
        // RPCS3 holds back every hit after a second cymbal; only the cymbal
        // waits here, and its pulse starts once the cymbal ahead has finished
        if (settings_.stagger_cymbals && cymbal && p.IsCymbal()) {
            p.waiting = true;
            continue;
        }
        if (p.waiting) {
            p.waiting = false;
            p.began = now;
        }
        cymbal |= p.IsCymbal();
        Apply(in, p.part, p.velocity);
        if (p.start) in.nav.start = true;
        if (p.select) in.nav.back = true;
    }
    if (hold_kick_) in.kick1 = true;
    return in;
}

}
