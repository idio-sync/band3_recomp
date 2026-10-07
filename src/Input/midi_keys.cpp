#include "midi_keys.h"
#include <algorithm>
#include <utility>

namespace band3::input::midi_keys {

namespace {

using namespace std::chrono_literals;

// a tap shorter than a game read isn't lost (RB3 reads about once a frame)
constexpr auto kMinPress = 30ms;
// RB3's menus look at the buttons once a frame, which a minimized game draws
// about 30 times a second, so a button's tap is held longer: 30 ms dropped
// some. 100 ms covers a frame of 10 fps, as band3's own Start presses do
// (song_pause.h).
constexpr auto kMinButtonPress = 100ms;
// the lowest and highest C held this long pause the song
constexpr auto kChordHold = 1s;
// how long the chord holds Start
constexpr auto kChordStart = 100ms;
constexpr int kChordLow = 0;
constexpr int kChordHigh = kKeyCount - 1;

// controllers, and how far each must go to count as pressed
constexpr uint8_t kModWheel = 1;
constexpr uint8_t kSustainPedal = 64;
constexpr uint8_t kControlDown = 64;
// pitch bend is 0x0000-0x3FFF with 0x2000 at rest; more than half way either way
constexpr uint16_t kBendLow = 0x1000;
constexpr uint16_t kBendHigh = 0x3000;

struct MenuKey {
    const char* name;
    // nullptr for the keys that do nothing in menus
    bool NavInputs::*button;
};

// the lowest octave in menus, C to B
constexpr MenuKey kMenuKeys[] = {
    {"Left", &NavInputs::dpad_left},  {"Y", &NavInputs::y},
    {"Down", &NavInputs::dpad_down},  {"X", &NavInputs::x},
    {"Up", &NavInputs::dpad_up},      {"Right", &NavInputs::dpad_right},
    {"Back", &NavInputs::back},       {"A", &NavInputs::a},
    {"unused in menus", nullptr},     {"B", &NavInputs::b},
    {"unused in menus", nullptr},     {"Start", &NavInputs::start},
};
constexpr int kMenuKeyCount = static_cast<int>(std::size(kMenuKeys));

}

Keyboard::Keyboard(const Settings& settings) : settings_(settings) {}

void Keyboard::SetSettings(const Settings& settings) {
    if (settings.base_note != settings_.base_note) {
        keys_.fill({});
        chord_fired_ = false;
    }
    settings_ = settings;
}

void Keyboard::Release(int key) {
    keys_[key].held = false;
    // the chord can fire again once it's formed again
    if (key == kChordLow || key == kChordHigh) chord_fired_ = false;
}

std::optional<Event> Keyboard::Receive(std::span<const uint8_t> message, Clock::time_point now,
                                       Mode mode) {
    if (message.size() < 3) return std::nullopt;
    Event event;
    std::copy_n(message.begin(), event.message.size(), event.message.begin());
    const uint8_t type = message[0] & 0xF0;
    const uint8_t data1 = message[1] & 0x7F;
    const uint8_t data2 = message[2] & 0x7F;

    const bool overdrive = mod_wheel_ || pitch_bend_;
    auto overdrive_changed = [&]() -> std::optional<Event> {
        if ((mod_wheel_ || pitch_bend_) == overdrive) return std::nullopt;
        event.what = overdrive ? "overdrive off" : "overdrive on";
        return event;
    };

    switch (type) {
    case 0x80:
    case 0x90: {
        // note-on and note-off on any channel; velocity 0 is a note-off
        const int key = data1 - settings_.base_note;
        const bool on = type == 0x90 && data2 > 0;
        if (key < 0 || key >= kKeyCount) {
            if (!on) return std::nullopt;
            event.what = "outside the 25 keys";
            return event;
        }
        Press& press = keys_[key];
        if (!on) {
            if (press.held) Release(key);
            return std::nullopt;
        }

        // a key already down keeps the meaning of the note-on that pressed it
        if (!press.held) {
            press.meaning =
                mode == Mode::kMenus && key < kMenuKeyCount ? Meaning::kButton : Meaning::kKey;
            press.held = true;
            press.began = now;
        }
        press.velocity = data2;
        press.last_on = now;
        event.what = press.meaning == Meaning::kButton ? kMenuKeys[key].name
                                                       : "key " + std::to_string(key);
        return event;
    }
    case 0xB0:
        if (data1 == kModWheel) {
            mod_wheel_ = data2 >= kControlDown;
            return overdrive_changed();
        }
        if (data1 == kSustainPedal) {
            const bool was_down = pedal_;
            pedal_ = data2 >= kControlDown;
            if (!pedal_ || was_down) return std::nullopt;
            event.what = "Start (pedal)";
            return event;
        }
        return std::nullopt;
    case 0xE0: {
        const int bend = (data2 << 7) | data1;
        pitch_bend_ = bend < kBendLow || bend > kBendHigh;
        return overdrive_changed();
    }
    default:
        return std::nullopt;
    }
}

KeysInputs Keyboard::State(Clock::time_point now) {
    KeysInputs in;
    for (int key = 0; key < kKeyCount; key++) {
        Press& press = keys_[key];
        if (press.meaning == Meaning::kUp) continue;
        const auto min_press = press.meaning == Meaning::kButton ? kMinButtonPress : kMinPress;
        if (!press.held && now >= press.last_on + min_press) {
            press = {};
            continue;
        }
        if (press.meaning == Meaning::kKey) {
            in.keys[key] = press.velocity;
        } else if (auto button = kMenuKeys[key].button) {
            in.nav.*button = true;
        }
    }

    // the pause chord: both Cs held as keys for a second, from the later one
    const Press& low = keys_[kChordLow];
    const Press& high = keys_[kChordHigh];
    const bool chord = low.held && low.meaning == Meaning::kKey && high.held &&
                       high.meaning == Meaning::kKey;
    if (chord && !chord_fired_ && now >= std::max(low.began, high.began) + kChordHold) {
        chord_fired_ = true;
        chord_start_until_ = now + kChordStart;
        timed_event_ = Event{{}, "pause"};
    }

    if (pedal_ || now < chord_start_until_) in.nav.start = true;
    in.overdrive = mod_wheel_ || pitch_bend_;
    return in;
}

std::optional<Event> Keyboard::TakeTimedEvent() { return std::exchange(timed_event_, std::nullopt); }

std::optional<int> NoteOf(const std::array<uint8_t, 3>& message) {
    const uint8_t type = message[0] & 0xF0;
    if (type != 0x80 && type != 0x90) return std::nullopt;
    return message[1] & 0x7F;
}

std::string NoteName(int note) {
    static constexpr const char* kNames[12] = {"C",  "C#", "D",  "D#", "E",  "F",
                                               "F#", "G",  "G#", "A",  "A#", "B"};
    note = std::clamp(note, 0, 127);
    return std::string(kNames[note % 12]) + std::to_string(note / 12 - 1);
}

}
