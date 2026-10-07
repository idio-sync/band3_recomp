// Checks MIDI keyboards played as RB3's keytar (src/Input/midi_keys.cpp): the
// 25 keys from the base note, the minimum press, the lowest octave as menu
// buttons, the pause chord, overdrive from the mod wheel or pitch bend, the
// sustain pedal as Start, and the notes' names.

#include <doctest/doctest.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include "src/Input/midi_keys.h"

using namespace band3::input;
using namespace band3::input::midi_keys;
using namespace std::chrono_literals;

namespace {

const Clock::time_point t0{};

constexpr Mode kPlaying = Mode::kPlaying;
constexpr Mode kMenus = Mode::kMenus;

// the default base note, the keytar's lowest C
constexpr uint8_t kBase = 48;

std::array<uint8_t, 3> NoteOn(uint8_t note, uint8_t velocity = 100, uint8_t channel = 0) {
    return {static_cast<uint8_t>(0x90 | channel), note, velocity};
}

std::array<uint8_t, 3> NoteOff(uint8_t note, uint8_t channel = 0) {
    return {static_cast<uint8_t>(0x80 | channel), note, 64};
}

std::array<uint8_t, 3> Control(uint8_t controller, uint8_t value) {
    return {0xB0, controller, value};
}

std::array<uint8_t, 3> PitchBend(uint16_t value) {
    return {0xE0, static_cast<uint8_t>(value & 0x7F), static_cast<uint8_t>(value >> 7)};
}

// the note of keytar key `key` at the default base note
uint8_t Key(int key) { return static_cast<uint8_t>(kBase + key); }

std::string What(const std::optional<Event>& event) { return event ? event->what : "(none)"; }

}

TEST_CASE("midi keys: the 25 keys start at the base note, on any channel") {
    Keyboard keyboard({});
    auto event = keyboard.Receive(NoteOn(Key(0), 90, 3), t0, kPlaying);
    CHECK(What(event) == "key 0");
    CHECK(event->message == NoteOn(Key(0), 90, 3));
    CHECK(What(keyboard.Receive(NoteOn(Key(7), 50), t0, kPlaying)) == "key 7");
    CHECK(What(keyboard.Receive(NoteOn(Key(24), 127, 15), t0, kPlaying)) == "key 24");

    const KeysInputs in = keyboard.State(t0 + 1ms);
    CHECK(in.keys[0] == 90);
    CHECK(in.keys[7] == 50);
    CHECK(in.keys[24] == 127);
    CHECK(in.keys[1] == 0);
    CHECK_FALSE(in.overdrive);
    CHECK_FALSE(in.nav.start);
}

TEST_CASE("midi keys: notes outside the 25 keys are reported but play nothing") {
    Keyboard keyboard({});
    CHECK(What(keyboard.Receive(NoteOn(kBase - 1), t0, kPlaying)) == "outside the 25 keys");
    CHECK(What(keyboard.Receive(NoteOn(kBase + 25), t0, kPlaying)) == "outside the 25 keys");
    CHECK(What(keyboard.Receive(NoteOn(0), t0, kMenus)) == "outside the 25 keys");
    CHECK(What(keyboard.Receive(NoteOn(127), t0, kMenus)) == "outside the 25 keys");
    CHECK_FALSE(keyboard.Receive(NoteOff(kBase - 1), t0, kPlaying).has_value());

    const KeysInputs in = keyboard.State(t0 + 1ms);
    for (int k = 0; k < kKeyCount; k++) CHECK(in.keys[k] == 0);
    CHECK(in.nav.a == false);
}

TEST_CASE("midi keys: a note-off or a velocity 0 note-on releases, without an event") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(3)), t0, kPlaying);
    keyboard.Receive(NoteOn(Key(5)), t0, kPlaying);
    CHECK_FALSE(keyboard.Receive(NoteOff(Key(3)), t0 + 100ms, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(NoteOn(Key(5), 0), t0 + 100ms, kPlaying).has_value());

    const KeysInputs in = keyboard.State(t0 + 101ms);
    CHECK(in.keys[3] == 0);
    CHECK(in.keys[5] == 0);
}

TEST_CASE("midi keys: a key stays down until held, then released") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(10), 80), t0, kPlaying);
    CHECK(keyboard.State(t0 + 10ms).keys[10] == 80);
    CHECK(keyboard.State(t0 + 5s).keys[10] == 80);
    keyboard.Receive(NoteOff(Key(10)), t0 + 5s, kPlaying);
    CHECK(keyboard.State(t0 + 5s + 1ms).keys[10] == 0);
}

TEST_CASE("midi keys: a tap shorter than a read is held 30 ms") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(2), 70), t0, kPlaying);
    keyboard.Receive(NoteOff(Key(2)), t0 + 2ms, kPlaying);
    // the game's next read comes after the release
    CHECK(keyboard.State(t0 + 16ms).keys[2] == 70);
    CHECK(keyboard.State(t0 + 29ms).keys[2] == 70);
    CHECK(keyboard.State(t0 + 30ms).keys[2] == 0);

}

TEST_CASE("midi keys: a button's tap is held 100 ms") {
    // RB3's menus look once a frame, and a minimized game draws ~30 a second
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(7)), t0, kMenus);
    keyboard.Receive(NoteOff(Key(7)), t0 + 1ms, kMenus);
    CHECK(keyboard.State(t0 + 30ms).nav.a);
    CHECK(keyboard.State(t0 + 99ms).nav.a);
    CHECK_FALSE(keyboard.State(t0 + 100ms).nav.a);

    // a button held longer goes when its key does
    keyboard.Receive(NoteOn(Key(2)), t0 + 1s, kMenus);
    CHECK(keyboard.State(t0 + 1s + 499ms).nav.dpad_down);
    keyboard.Receive(NoteOff(Key(2)), t0 + 1s + 500ms, kMenus);
    CHECK_FALSE(keyboard.State(t0 + 1s + 501ms).nav.dpad_down);
}

TEST_CASE("midi keys: a note-on for a key already down keeps it down with the new velocity") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(4), 40), t0, kPlaying);
    CHECK(What(keyboard.Receive(NoteOn(Key(4), 110), t0 + 50ms, kPlaying)) == "key 4");
    CHECK(keyboard.State(t0 + 60ms).keys[4] == 110);
    // one note-off releases it, held 30 ms from the later note-on
    keyboard.Receive(NoteOff(Key(4)), t0 + 60ms, kPlaying);
    CHECK(keyboard.State(t0 + 79ms).keys[4] == 110);
    CHECK(keyboard.State(t0 + 80ms).keys[4] == 0);
}

TEST_CASE("midi keys: in menus the lowest octave is buttons") {
    struct Case {
        int key;
        const char* what;
        bool NavInputs::*button;
    };
    const Case cases[] = {
        {0, "Left", &NavInputs::dpad_left}, {1, "Y", &NavInputs::y},
        {2, "Down", &NavInputs::dpad_down}, {3, "X", &NavInputs::x},
        {4, "Up", &NavInputs::dpad_up},     {5, "Right", &NavInputs::dpad_right},
        {6, "Back", &NavInputs::back},      {7, "A", &NavInputs::a},
        {9, "B", &NavInputs::b},            {11, "Start", &NavInputs::start},
    };
    for (const auto& c : cases) {
        CAPTURE(c.key);
        Keyboard keyboard({});
        CHECK(What(keyboard.Receive(NoteOn(Key(c.key)), t0, kMenus)) == c.what);
        KeysInputs in = keyboard.State(t0 + 1ms);
        CHECK(in.nav.*c.button);
        CHECK(in.keys[c.key] == 0);  // a button, not a key

        // held while its key is
        CHECK(keyboard.State(t0 + 2s).nav.*c.button);
        keyboard.Receive(NoteOff(Key(c.key)), t0 + 2s, kMenus);
        CHECK_FALSE(keyboard.State(t0 + 2s + 1ms).nav.*c.button);
    }
}

TEST_CASE("midi keys: G# and A# do nothing in menus") {
    for (int key : {8, 10}) {
        CAPTURE(key);
        Keyboard keyboard({});
        CHECK(What(keyboard.Receive(NoteOn(Key(key)), t0, kMenus)) == "unused in menus");
        const KeysInputs in = keyboard.State(t0 + 1ms);
        CHECK(in.keys[key] == 0);
        const NavInputs none;
        CHECK(std::memcmp(&in.nav, &none, sizeof(none)) == 0);
    }
}

TEST_CASE("midi keys: in menus the upper keys still play, and in a song every key plays") {
    Keyboard keyboard({});
    CHECK(What(keyboard.Receive(NoteOn(Key(12), 60), t0, kMenus)) == "key 12");
    CHECK(What(keyboard.Receive(NoteOn(Key(24), 61), t0, kMenus)) == "key 24");
    KeysInputs in = keyboard.State(t0 + 1ms);
    CHECK(in.keys[12] == 60);
    CHECK(in.keys[24] == 61);

    Keyboard playing({});
    for (int k = 0; k < 12; k++) {
        CAPTURE(k);
        CHECK(What(playing.Receive(NoteOn(Key(k), 90), t0, kPlaying)) ==
              "key " + std::to_string(k));
    }
    in = playing.State(t0 + 1ms);
    for (int k = 0; k < 12; k++) CHECK(in.keys[k] == 90);
    const NavInputs none;
    CHECK(std::memcmp(&in.nav, &none, sizeof(none)) == 0);
}

TEST_CASE("midi keys: a key keeps the meaning of its note-on across a change of mode") {
    Keyboard keyboard({});
    // a button pressed in menus stays a button when the song starts
    keyboard.Receive(NoteOn(Key(7), 90), t0, kMenus);
    // a key pressed in the song stays a key when it pauses
    keyboard.Receive(NoteOn(Key(0), 80), t0, kPlaying);

    // a note-off arriving in the other mode releases each all the same
    KeysInputs in = keyboard.State(t0 + 1s);
    CHECK(in.nav.a);
    CHECK(in.keys[7] == 0);
    CHECK(in.keys[0] == 80);
    CHECK_FALSE(in.nav.dpad_left);

    keyboard.Receive(NoteOff(Key(7)), t0 + 1s, kPlaying);
    keyboard.Receive(NoteOff(Key(0)), t0 + 1s, kMenus);
    in = keyboard.State(t0 + 1s + 1ms);
    CHECK_FALSE(in.nav.a);
    CHECK(in.keys[0] == 0);

    // pressed again, each takes the mode of its new note-on
    CHECK(What(keyboard.Receive(NoteOn(Key(7)), t0 + 2s, kPlaying)) == "key 7");
    CHECK(What(keyboard.Receive(NoteOn(Key(0)), t0 + 2s, kMenus)) == "Left");
}

TEST_CASE("midi keys: the lowest and highest C held for a second pause, once per hold") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(0), 100), t0, kPlaying);
    keyboard.Receive(NoteOn(Key(24), 100), t0 + 200ms, kPlaying);

    // a second from the later of the two
    KeysInputs in = keyboard.State(t0 + 1100ms);
    CHECK_FALSE(in.nav.start);
    CHECK_FALSE(keyboard.TakeTimedEvent().has_value());
    // the two still play as keys
    CHECK(in.keys[0] == 100);
    CHECK(in.keys[24] == 100);

    in = keyboard.State(t0 + 1200ms);
    CHECK(in.nav.start);
    auto event = keyboard.TakeTimedEvent();
    CHECK(What(event) == "pause");
    CHECK(event->message == std::array<uint8_t, 3>{});
    CHECK_FALSE(keyboard.TakeTimedEvent().has_value());  // reported once
    CHECK(in.keys[0] == 100);
    CHECK(in.keys[24] == 100);

    // Start for 100 ms
    CHECK(keyboard.State(t0 + 1299ms).nav.start);
    CHECK_FALSE(keyboard.State(t0 + 1300ms).nav.start);

    // held on, it doesn't fire again
    CHECK_FALSE(keyboard.State(t0 + 3s).nav.start);
    CHECK_FALSE(keyboard.State(t0 + 10s).nav.start);
    CHECK_FALSE(keyboard.TakeTimedEvent().has_value());

    // after one is released, the chord formed again fires again
    keyboard.Receive(NoteOff(Key(24)), t0 + 11s, kMenus);
    keyboard.Receive(NoteOn(Key(24)), t0 + 12s, kPlaying);
    CHECK_FALSE(keyboard.State(t0 + 12900ms).nav.start);
    CHECK(keyboard.State(t0 + 13s).nav.start);
    CHECK(What(keyboard.TakeTimedEvent()) == "pause");
}

TEST_CASE("midi keys: the pause chord needs both keys held as keys") {
    // the lowest C pressed in menus is Left, not the chord's key
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(0)), t0, kMenus);
    keyboard.Receive(NoteOn(Key(24)), t0, kPlaying);
    CHECK_FALSE(keyboard.State(t0 + 2s).nav.start);
    CHECK_FALSE(keyboard.TakeTimedEvent().has_value());

    // a key released inside its 30 ms isn't held
    Keyboard tapped({});
    tapped.Receive(NoteOn(Key(0)), t0, kPlaying);
    tapped.Receive(NoteOn(Key(24)), t0, kPlaying);
    tapped.Receive(NoteOff(Key(0)), t0 + 999ms, kPlaying);
    CHECK_FALSE(tapped.State(t0 + 1s).nav.start);

    // nor does a repeated note-on restart the second
    Keyboard repeated({});
    repeated.Receive(NoteOn(Key(0)), t0, kPlaying);
    repeated.Receive(NoteOn(Key(24)), t0, kPlaying);
    repeated.Receive(NoteOn(Key(24), 50), t0 + 900ms, kPlaying);
    CHECK(repeated.State(t0 + 1s).nav.start);
}

TEST_CASE("midi keys: overdrive from the mod wheel") {
    Keyboard keyboard({});
    CHECK_FALSE(keyboard.Receive(Control(1, 63), t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.State(t0).overdrive);

    auto event = keyboard.Receive(Control(1, 64), t0, kPlaying);
    CHECK(What(event) == "overdrive on");
    CHECK(event->message == Control(1, 64));
    CHECK(keyboard.State(t0 + 1s).overdrive);
    // further up, still on, nothing new to report
    CHECK_FALSE(keyboard.Receive(Control(1, 127), t0 + 1s, kPlaying).has_value());
    CHECK(keyboard.State(t0 + 1s).overdrive);

    CHECK(What(keyboard.Receive(Control(1, 10), t0 + 2s, kMenus)) == "overdrive off");
    CHECK_FALSE(keyboard.State(t0 + 2s).overdrive);
}

TEST_CASE("midi keys: overdrive from the pitch bend, either way") {
    Keyboard keyboard({});
    // centre and half way either side are off
    CHECK_FALSE(keyboard.Receive(PitchBend(0x2000), t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(PitchBend(0x1000), t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(PitchBend(0x3000), t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.State(t0).overdrive);

    CHECK(What(keyboard.Receive(PitchBend(0x3001), t0, kPlaying)) == "overdrive on");
    CHECK(keyboard.State(t0).overdrive);
    CHECK_FALSE(keyboard.Receive(PitchBend(0x3FFF), t0, kPlaying).has_value());
    CHECK(What(keyboard.Receive(PitchBend(0x2000), t0, kPlaying)) == "overdrive off");
    CHECK_FALSE(keyboard.State(t0).overdrive);

    CHECK(What(keyboard.Receive(PitchBend(0x0FFF), t0, kPlaying)) == "overdrive on");
    CHECK(keyboard.State(t0).overdrive);
    CHECK(What(keyboard.Receive(PitchBend(0x0000), t0, kPlaying)) == "(none)");
    CHECK(keyboard.State(t0).overdrive);
    CHECK(What(keyboard.Receive(PitchBend(0x1FFF), t0, kPlaying)) == "overdrive off");
}

TEST_CASE("midi keys: overdrive is held while either the wheel or the bend holds it") {
    Keyboard keyboard({});
    CHECK(What(keyboard.Receive(Control(1, 100), t0, kPlaying)) == "overdrive on");
    CHECK_FALSE(keyboard.Receive(PitchBend(0x3FFF), t0, kPlaying).has_value());
    // the wheel let go; the bend still holds it
    CHECK_FALSE(keyboard.Receive(Control(1, 0), t0, kPlaying).has_value());
    CHECK(keyboard.State(t0).overdrive);
    CHECK(What(keyboard.Receive(PitchBend(0x2000), t0, kPlaying)) == "overdrive off");
    CHECK_FALSE(keyboard.State(t0).overdrive);
}

TEST_CASE("midi keys: the sustain pedal holds Start, in either mode") {
    for (Mode mode : {kPlaying, kMenus}) {
        Keyboard keyboard({});
        CHECK_FALSE(keyboard.Receive(Control(64, 63), t0, mode).has_value());
        CHECK_FALSE(keyboard.State(t0).nav.start);

        auto event = keyboard.Receive(Control(64, 127), t0, mode);
        CHECK(What(event) == "Start (pedal)");
        CHECK(event->message == Control(64, 127));
        CHECK(keyboard.State(t0 + 1s).nav.start);
        // already down: nothing new
        CHECK_FALSE(keyboard.Receive(Control(64, 100), t0 + 1s, mode).has_value());

        // released: no event, and Start lets go
        CHECK_FALSE(keyboard.Receive(Control(64, 0), t0 + 2s, mode).has_value());
        CHECK_FALSE(keyboard.State(t0 + 2s).nav.start);
    }
}

TEST_CASE("midi keys: other messages are ignored") {
    Keyboard keyboard({});
    const std::array<uint8_t, 3> other_control{0xB0, 7, 127};   // volume
    const std::array<uint8_t, 3> aftertouch{0xA0, Key(3), 100};  // polyphonic pressure
    const std::array<uint8_t, 2> program{0xC0, 5};
    const std::array<uint8_t, 2> short_note{0x90, Key(3)};
    const std::array<uint8_t, 1> lone{0xF8};
    CHECK_FALSE(keyboard.Receive(other_control, t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(aftertouch, t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(program, t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(short_note, t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(lone, t0, kPlaying).has_value());
    CHECK_FALSE(keyboard.Receive(std::span<const uint8_t>{}, t0, kPlaying).has_value());

    const KeysInputs in = keyboard.State(t0 + 1ms);
    CHECK(in.keys[3] == 0);
    CHECK_FALSE(in.overdrive);
}

TEST_CASE("midi keys: a new base note moves the keys and releases those held") {
    Keyboard keyboard({});
    keyboard.Receive(NoteOn(Key(5), 90), t0, kPlaying);
    keyboard.Receive(NoteOn(Key(2), 90), t0, kMenus);  // Down
    KeysInputs in = keyboard.State(t0 + 1ms);
    REQUIRE(in.keys[5] == 90);
    REQUIRE(in.nav.dpad_down);

    // the launcher's slider dragged while a key is down
    Settings higher;
    higher.base_note = 60;
    keyboard.SetSettings(higher);
    in = keyboard.State(t0 + 2ms);
    CHECK(in.keys[5] == 0);
    CHECK_FALSE(in.nav.dpad_down);

    // the old note-off finds nothing of its own to release
    keyboard.Receive(NoteOn(60 + 5, 70), t0 + 1s, kPlaying);
    keyboard.Receive(NoteOff(Key(5)), t0 + 1s, kPlaying);
    in = keyboard.State(t0 + 1s + 1ms);
    CHECK(in.keys[5] == 70);
    CHECK(What(keyboard.Receive(NoteOn(Key(0)), t0 + 2s, kPlaying)) == "outside the 25 keys");
    CHECK(What(keyboard.Receive(NoteOn(84), t0 + 2s, kPlaying)) == "key 24");

    // the same base note again releases nothing
    keyboard.SetSettings(higher);
    CHECK(keyboard.State(t0 + 3s).keys[5] == 70);
}

TEST_CASE("midi keys: a new base note doesn't touch the pedal or overdrive") {
    Keyboard keyboard({});
    keyboard.Receive(Control(64, 127), t0, kPlaying);
    keyboard.Receive(Control(1, 127), t0, kPlaying);
    Settings lower;
    lower.base_note = 36;
    keyboard.SetSettings(lower);
    const KeysInputs in = keyboard.State(t0 + 1ms);
    CHECK(in.nav.start);
    CHECK(in.overdrive);
}

TEST_CASE("midi keys: notes are named in scientific pitch, middle C C4") {
    CHECK(NoteName(60) == "C4");
    CHECK(NoteName(48) == "C3");
    CHECK(NoteName(61) == "C#4");
    CHECK(NoteName(59) == "B3");
    CHECK(NoteName(70) == "A#4");
    CHECK(NoteName(0) == "C-1");
    CHECK(NoteName(11) == "B-1");
    CHECK(NoteName(12) == "C0");
    CHECK(NoteName(103) == "G7");
    CHECK(NoteName(127) == "G9");
}

TEST_CASE("midi keys: an event's note is its note-on's or note-off's, on any channel") {
    CHECK(NoteOf(NoteOn(60)) == 60);
    CHECK(NoteOf(NoteOn(48, 100, 9)) == 48);
    CHECK(NoteOf(NoteOff(72)) == 72);
    CHECK_FALSE(NoteOf(Control(1, 127)).has_value());
    CHECK_FALSE(NoteOf(PitchBend(0)).has_value());
    // the pause chord's event, which no message fires
    CHECK_FALSE(NoteOf({}).has_value());
}
