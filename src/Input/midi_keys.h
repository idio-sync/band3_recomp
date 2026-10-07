#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include "instruments.h"

// A MIDI keyboard played as RB3's keytar, for Keys and Pro Keys, with no other
// controller: 25 keys from a base note play the keytar's C to C, and the rest
// of what a player needs comes from the keyboard too.
//
// - Menus. Outside a song (or with it paused) the lowest octave is the menu
//   buttons, C to B: Left, Y, Down, X, Up, Right, Back, A, (none), B, (none),
//   Start: the white keys are the d-pad, A, B and Start, and the black keys
//   the rarer Y, X and Back. The upper 13 keys still play keys, which the
//   menus ignore. In a song every key plays.
// - Each key's meaning is fixed at its note-on and kept until its note-off, so
//   a key held across a change of mode neither turns into a button nor stops
//   being one: holding the pause chord doesn't leave Left held in the pause
//   menu, and A held as a song starts doesn't play a key.
// - Pause. In a song the lowest and highest C held together for a second press
//   Start for 100 ms, once per hold. A keyboard has no Start of its own and the
//   lowest octave plays in a song; no chart asks for both Cs at once, so the
//   chord costs at worst an overhit. They still play as keys while held.
// - Overdrive while the mod wheel is at least half way up, or the pitch bend
//   more than half way from centre either way: whichever the keyboard has.
// - The sustain pedal is Start, held while it is, in a song or not.
// - A press is held at least 30 ms after its note-on, even if released sooner,
//   so a tap shorter than a game read isn't lost (RB3 reads about once a
//   frame); a button's at least 100 ms, since the menus look once a frame and
//   a minimized game draws only about 30 a second.
//
// Time is passed in rather than read, so this is testable; the MIDI driver
// (midi_keys_driver.h) feeds it messages and asks for the state.

namespace band3::input::midi_keys {

using Clock = std::chrono::steady_clock;

// What a note-on means. Playing is a song on a gameplay screen and not paused
// (practice included); menus is everything else.
enum class Mode { kPlaying, kMenus };

struct Settings {
    // the MIDI note of the keytar's lowest C (48 = C3, an octave below middle C)
    uint8_t base_note = 48;
};

// what a received message (or the pause chord) did, for the Instrument Lab
struct Event {
    // the message's bytes; zero for the pause chord, which no message fires
    std::array<uint8_t, 3> message{};
    // "key 7", "Left", "unused in menus", "outside the 25 keys", "overdrive
    // on", "overdrive off", "Start (pedal)" or "pause"
    std::string what;
};

class Keyboard {
public:
    explicit Keyboard(const Settings& settings);

    // One MIDI message received at `now`, in `mode`, which decides what a
    // note-on means. Returns what it did: each note-on, the pedal going down
    // and overdrive turning on or off. Releases and other messages return
    // nothing.
    std::optional<Event> Receive(std::span<const uint8_t> message, Clock::time_point now,
                                 Mode mode);

    // what the keyboard is pressing at `now`
    KeysInputs State(Clock::time_point now);

    // the pause chord firing, once, after the State call that fired it
    std::optional<Event> TakeTimedEvent();

    // Changes the settings from here on. A new base note releases every key
    // and button held at once: their note-offs would land on other keys.
    void SetSettings(const Settings& settings);

private:
    // what a key does while it's down, fixed at its note-on
    enum class Meaning : uint8_t { kUp, kKey, kButton };

    struct Press {
        Meaning meaning = Meaning::kUp;
        uint8_t velocity = 0;
        // between its note-on and note-off
        bool held = false;
        // the note-on that pressed it, for the pause chord
        Clock::time_point began{};
        // the latest note-on, which it stays down at least 30 ms after (100 ms
        // for a button)
        Clock::time_point last_on{};
    };

    void Release(int key);

    Settings settings_;
    std::array<Press, kKeyCount> keys_{};
    bool mod_wheel_ = false;
    bool pitch_bend_ = false;
    bool pedal_ = false;
    // the chord has paused for this hold
    bool chord_fired_ = false;
    Clock::time_point chord_start_until_{};
    std::optional<Event> timed_event_;
};

// the note an event's note-on or note-off is of; nullopt for other messages
std::optional<int> NoteOf(const std::array<uint8_t, 3>& message);

// A MIDI note's name in scientific pitch, sharps with '#': 60 (middle C) is
// "C4", 48 "C3", 61 "C#4", 0 "C-1". For the base note's labels; MIDI's notes
// are 0-127.
std::string NoteName(int note);

}
