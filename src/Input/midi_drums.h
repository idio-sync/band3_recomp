#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include "instruments.h"

// A MIDI drum kit played as a Rock Band drum kit, without a MIDI Pro Adapter.
// Adapted from RPCS3's emulated MIDI Pro Adapter (rpcs3/Emu/Io/RB3MidiDrums.cpp,
// GPL-2.0): the same note table (which follows the MIDI Pro Adapter's), hit
// pulses, minimum velocity, staggered cymbals and menu-button combos, and the
// same "note=Part" override format, so RPCS3 overrides carry over.
//
// Time is passed in rather than read, so this is testable; the MIDI driver
// (midi_drums_driver.h) feeds it messages and asks for the state.

namespace band3::input::midi_drums {

using Clock = std::chrono::steady_clock;

enum class Part : uint8_t {
    kNone,
    kKick,
    kHihatPedal,  // the second pedal; RB3 decides whether that's a kick
    kSnare,
    kSnareRim,    // a snare hit, told apart only for the select combo
    kHiTom,
    kLowTom,
    kFloorTom,
    kHihat,
    kRide,
    kCrash,
};

// the names overrides and combos use, as in RPCS3 ("Kick", "HihatPedal", ...)
const char* PartName(Part part);
std::optional<Part> ParsePart(std::string_view name);

// which part each MIDI note plays
using NoteMap = std::array<Part, 128>;

// the MIDI Pro Adapter's note table, as RPCS3 has it
NoteMap DefaultNoteMap();

// Applies "note=Part" overrides, comma separated (e.g. "40=Snare,44=Kick").
// Returns the entries it couldn't read, to report.
std::vector<std::string> ApplyOverrides(NoteMap& map, std::string_view overrides);

struct Settings {
    // How long each hit is held. RB3's joypad thread polls every 4 ms and keeps
    // a press until the next frame reads it, so a short hit isn't missed; the
    // length is for stagger_cymbals, whose second cymbal starts as the first
    // one's pulse ends and needs a frame to read in between. Shorter than a
    // frame (16.7 ms at 60 fps), both cymbals land in one read.
    std::chrono::milliseconds pulse{30};
    // quieter hits are ignored
    uint8_t min_velocity = 10;
    // two cymbals at once are played one pulse apart, since the 360 cymbal
    // flags can't say which two it was
    bool stagger_cymbals = true;
    // hi-hat pedal x3 then snare = Start, then rim = Select, then kick =
    // toggle a held kick (for RB3's song category menu)
    bool combos = true;
    std::chrono::milliseconds combo_window{2000};
};

// what a received MIDI message did, for the Instrument Lab
struct Hit {
    uint8_t note = 0;
    uint8_t velocity = 0;
    Part part = Part::kNone;
    // "start", "select" or "hold kick" when this completed a combo
    const char* combo = nullptr;
};

class Kit {
public:
    Kit(const NoteMap& notes, const Settings& settings);

    // One MIDI message received at `now`. Returns what it hit, if it was a
    // note-on the kit maps; other messages are ignored.
    std::optional<Hit> Receive(std::span<const uint8_t> message, Clock::time_point now);

    // what the kit is pressing at `now`
    DrumInputs State(Clock::time_point now);

    // Changes the settings from here on, keeping the hits still sounding; with
    // combos turned off, a combo in progress is dropped.
    void SetSettings(const Settings& settings);

private:
    struct Pulse {
        // when it started sounding; it lasts settings_.pulse from then
        Clock::time_point began;
        Part part = Part::kNone;
        uint8_t velocity = 0;
        bool start = false;
        bool select = false;
        // a cymbal held back behind another; its pulse starts when it plays
        bool waiting = false;
        bool IsCymbal() const;
    };

    // returns the combo `part` completes, if any
    const char* TrackCombo(Part part, Clock::time_point now);

    NoteMap notes_;
    Settings settings_;
    std::vector<Pulse> pulses_;
    bool hold_kick_ = false;
    std::vector<Part> combo_;
    Clock::time_point combo_expiry_{};
};

}
