// Checks MIDI drum kits (src/Input/midi_drums.cpp): the MIDI Pro Adapter note
// table RPCS3 uses, overrides, hit pulses, staggered cymbals and the menu
// combos.

#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include "src/Input/midi_drums.h"

using namespace band3::input;
using namespace band3::input::midi_drums;
using namespace std::chrono_literals;

namespace {

const Clock::time_point t0{};

std::array<uint8_t, 3> NoteOn(uint8_t note, uint8_t velocity = 100, uint8_t channel = 9) {
    return {static_cast<uint8_t>(0x90 | channel), note, velocity};
}

Kit DefaultKit(Settings settings = {}) { return Kit(DefaultNoteMap(), settings); }

// kit notes
constexpr uint8_t kKickNote = 36, kHihatPedalNote = 44, kSnareNote = 38, kRimNote = 40,
                  kHiTomNote = 48, kLowTomNote = 45, kFloorTomNote = 41, kHihatNote = 42,
                  kRideNote = 51, kCrashNote = 49;

}

TEST_CASE("the default note table follows the MIDI Pro Adapter") {
    const NoteMap map = DefaultNoteMap();
    CHECK(map[36] == Part::kKick);
    CHECK(map[35] == Part::kKick);
    CHECK(map[44] == Part::kHihatPedal);
    CHECK(map[38] == Part::kSnare);
    CHECK(map[40] == Part::kSnareRim);
    CHECK(map[48] == Part::kHiTom);
    CHECK(map[45] == Part::kLowTom);
    CHECK(map[41] == Part::kFloorTom);
    CHECK(map[42] == Part::kHihat);
    CHECK(map[46] == Part::kHihat);  // open hi-hat
    CHECK(map[51] == Part::kRide);
    CHECK(map[49] == Part::kCrash);
    CHECK(map[60] == Part::kNone);
}

TEST_CASE("overrides use RPCS3's note=Part format") {
    NoteMap map = DefaultNoteMap();
    const auto bad = ApplyOverrides(
        map, "40=Snare, 44=Kick,46=HihatWithPedalUp,36=None,x=Kick,50=Nope,200=Kick,oops,");
    CHECK(map[40] == Part::kSnare);
    CHECK(map[44] == Part::kKick);
    CHECK(map[46] == Part::kHihat);
    CHECK(map[36] == Part::kNone);
    REQUIRE(bad.size() == 4);
    CHECK(bad[0] == "x=Kick");
    CHECK(bad[1] == "50=Nope");
    CHECK(bad[2] == "200=Kick");
    CHECK(bad[3] == "oops");
    CHECK(map[50] == Part::kHiTom);  // untouched by the bad entry
}

TEST_CASE("only note-ons are hits, on any channel") {
    Kit kit = DefaultKit();
    CHECK(kit.Receive(NoteOn(kSnareNote, 100, 9), t0).has_value());
    CHECK(kit.Receive(NoteOn(kSnareNote, 100, 0), t0).has_value());

    const std::array<uint8_t, 3> note_off{0x89, kSnareNote, 64};
    const std::array<uint8_t, 3> control{0xB9, 4, 127};
    const std::array<uint8_t, 2> short_message{0x99, kSnareNote};
    CHECK_FALSE(kit.Receive(note_off, t0).has_value());
    CHECK_FALSE(kit.Receive(control, t0).has_value());
    CHECK_FALSE(kit.Receive(short_message, t0).has_value());
    CHECK_FALSE(kit.Receive(NoteOn(kSnareNote, 0), t0).has_value());  // velocity 0 = off
}

TEST_CASE("unmapped and quiet notes are reported but not played") {
    Kit kit = DefaultKit();
    auto hit = kit.Receive(NoteOn(60), t0);
    REQUIRE(hit);
    CHECK(hit->part == Part::kNone);

    hit = kit.Receive(NoteOn(kSnareNote, 5), t0);
    REQUIRE(hit);
    CHECK(hit->part == Part::kSnare);
    CHECK(kit.State(t0).pads[kRedPad] == 0);
}

TEST_CASE("each part plays its Rock Band pad, cymbal or pedal") {
    struct Case {
        uint8_t note;
        void (*check)(const DrumInputs&);
    };
    const Case cases[] = {
        {kKickNote, [](const DrumInputs& in) { CHECK(in.kick1); }},
        {kHihatPedalNote, [](const DrumInputs& in) { CHECK(in.kick2); }},
        {kSnareNote, [](const DrumInputs& in) { CHECK(in.pads[kRedPad] == 100); }},
        {kRimNote, [](const DrumInputs& in) { CHECK(in.pads[kRedPad] == 100); }},
        {kHiTomNote, [](const DrumInputs& in) { CHECK(in.pads[kYellowPad] == 100); }},
        {kLowTomNote, [](const DrumInputs& in) { CHECK(in.pads[kBluePad] == 100); }},
        {kFloorTomNote, [](const DrumInputs& in) { CHECK(in.pads[kGreenPad] == 100); }},
        {kHihatNote, [](const DrumInputs& in) { CHECK(in.cymbals[kYellowCymbal] == 100); }},
        {kRideNote, [](const DrumInputs& in) { CHECK(in.cymbals[kBlueCymbal] == 100); }},
        {kCrashNote, [](const DrumInputs& in) { CHECK(in.cymbals[kGreenCymbal] == 100); }},
    };
    for (const auto& c : cases) {
        CAPTURE(c.note);
        Kit kit = DefaultKit();
        kit.Receive(NoteOn(c.note), t0);
        c.check(kit.State(t0 + 1ms));
    }
}

TEST_CASE("a hit is held for the pulse, then released") {
    Kit kit = DefaultKit();
    kit.Receive(NoteOn(kSnareNote, 90), t0);
    CHECK(kit.State(t0 + 10ms).pads[kRedPad] == 90);
    CHECK(kit.State(t0 + 29ms).pads[kRedPad] == 90);
    CHECK(kit.State(t0 + 30ms).pads[kRedPad] == 0);
}

TEST_CASE("two cymbals at once are played one after the other") {
    Kit kit = DefaultKit();
    kit.Receive(NoteOn(kCrashNote), t0);
    kit.Receive(NoteOn(kRideNote), t0);
    kit.Receive(NoteOn(kSnareNote), t0);

    DrumInputs in = kit.State(t0 + 1ms);
    CHECK(in.cymbals[kGreenCymbal] == 100);
    CHECK(in.cymbals[kBlueCymbal] == 0);  // waiting
    CHECK(in.pads[kRedPad] == 100);       // pads don't wait

    in = kit.State(t0 + 31ms);
    CHECK(in.cymbals[kGreenCymbal] == 0);
    CHECK(in.cymbals[kBlueCymbal] == 100);

    Settings together;
    together.stagger_cymbals = false;
    Kit unstaggered = DefaultKit(together);
    unstaggered.Receive(NoteOn(kCrashNote), t0);
    unstaggered.Receive(NoteOn(kRideNote), t0);
    in = unstaggered.State(t0 + 1ms);
    CHECK(in.cymbals[kGreenCymbal] == 100);
    CHECK(in.cymbals[kBlueCymbal] == 100);
}

TEST_CASE("hi-hat pedal three times, then a pad, presses a menu button") {
    Kit kit = DefaultKit();
    for (int i = 0; i < 3; i++) kit.Receive(NoteOn(kHihatPedalNote), t0 + i * 100ms);
    auto hit = kit.Receive(NoteOn(kSnareNote), t0 + 300ms);
    REQUIRE(hit);
    REQUIRE(hit->combo != nullptr);
    CHECK(std::string_view(hit->combo) == "start");
    DrumInputs in = kit.State(t0 + 301ms);
    CHECK(in.nav.start);
    CHECK(in.pads[kRedPad] == 0);  // the combo's snare isn't a hit

    for (int i = 0; i < 3; i++) kit.Receive(NoteOn(kHihatPedalNote), t0 + 1s + i * 100ms);
    hit = kit.Receive(NoteOn(kRimNote), t0 + 1300ms);
    REQUIRE(hit->combo != nullptr);
    CHECK(std::string_view(hit->combo) == "select");
    CHECK(kit.State(t0 + 1301ms).nav.back);
}

TEST_CASE("the hold kick combo holds the kick until snare or floor tom") {
    Kit kit = DefaultKit();
    for (int i = 0; i < 3; i++) kit.Receive(NoteOn(kHihatPedalNote), t0 + i * 100ms);
    auto hit = kit.Receive(NoteOn(kKickNote), t0 + 300ms);
    REQUIRE(hit->combo != nullptr);
    CHECK(std::string_view(hit->combo) == "hold kick");
    CHECK(kit.State(t0 + 5s).kick1);

    kit.Receive(NoteOn(kFloorTomNote), t0 + 6s);
    const DrumInputs in = kit.State(t0 + 6s + 1ms);
    CHECK_FALSE(in.kick1);
    CHECK(in.pads[kGreenPad] == 0);  // closing the menu isn't a hit
}

TEST_CASE("combos time out, restart after a stray note, and can be turned off") {
    Kit kit = DefaultKit();
    kit.Receive(NoteOn(kHihatPedalNote), t0);
    kit.Receive(NoteOn(kHihatPedalNote), t0 + 100ms);
    kit.Receive(NoteOn(kHihatPedalNote), t0 + 2500ms);  // after the 2 s window
    auto hit = kit.Receive(NoteOn(kSnareNote), t0 + 2600ms);
    CHECK(hit->combo == nullptr);

    // a crash breaks the combo; the pedals after it start a new one
    Kit restart = DefaultKit();
    restart.Receive(NoteOn(kHihatPedalNote), t0);
    restart.Receive(NoteOn(kCrashNote), t0 + 50ms);
    for (int i = 0; i < 3; i++) restart.Receive(NoteOn(kHihatPedalNote), t0 + 100ms + i * 50ms);
    hit = restart.Receive(NoteOn(kSnareNote), t0 + 300ms);
    REQUIRE(hit->combo != nullptr);
    CHECK(std::string_view(hit->combo) == "start");

    Settings off;
    off.combos = false;
    Kit plain = DefaultKit(off);
    for (int i = 0; i < 3; i++) plain.Receive(NoteOn(kHihatPedalNote), t0 + i * 10ms);
    hit = plain.Receive(NoteOn(kSnareNote), t0 + 40ms);
    CHECK(hit->combo == nullptr);
    CHECK(plain.State(t0 + 41ms).pads[kRedPad] == 100);
}

TEST_CASE("settings changed while the kit plays apply from the next hit") {
    Kit kit = DefaultKit();
    kit.Receive(NoteOn(kSnareNote, 20), t0);
    CHECK(kit.State(t0 + 1ms).pads[kRedPad] == 20);

    // the launcher's Minimum velocity slider, dragged while the kit runs
    Settings louder;
    louder.min_velocity = 50;
    louder.pulse = 60ms;
    kit.SetSettings(louder);
    kit.Receive(NoteOn(kHiTomNote, 20), t0 + 100ms);
    CHECK(kit.State(t0 + 101ms).pads[kYellowPad] == 0);
    kit.Receive(NoteOn(kHiTomNote, 80), t0 + 200ms);
    CHECK(kit.State(t0 + 250ms).pads[kYellowPad] == 80);
    CHECK(kit.State(t0 + 260ms).pads[kYellowPad] == 0);

    // turning combos off drops one in progress
    Kit combos = DefaultKit();
    for (int i = 0; i < 3; i++) combos.Receive(NoteOn(kHihatPedalNote), t0 + i * 10ms);
    Settings off;
    off.combos = false;
    combos.SetSettings(off);
    combos.SetSettings({});
    const auto hit = combos.Receive(NoteOn(kSnareNote), t0 + 40ms);
    CHECK(hit->combo == nullptr);
}
