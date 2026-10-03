#include "device_view_model.h"
#include <algorithm>
#include <cctype>
#include <cmath>

namespace band3::launcher {

using input::DeviceKind;
namespace xbox = input::xbox;

namespace {

// the instrument a subtype reports, or nullptr for a gamepad or one the
// launcher doesn't name
const char* InstrumentName(uint8_t sub_type) {
    switch (sub_type) {
    case input::kSubtypeGuitar:
    case input::kSubtypeGuitarAlternate:
    case input::kSubtypeGuitarBass:
        return "guitar";
    case input::kSubtypeDrums: return "drum kit";
    case input::kSubtypeKeytar: return "keytar";
    case input::kSubtypeProGuitar: return "Pro Guitar";
    default: return nullptr;
    }
}

std::string Capitalized(std::string s) {
    if (!s.empty() && s[0] >= 'a' && s[0] <= 'z') s[0] = static_cast<char>(s[0] - 'a' + 'A');
    return s;
}

// XInput's dead zones (XINPUT_GAMEPAD_LEFT_THUMB_DEADZONE,
// XINPUT_GAMEPAD_TRIGGER_THRESHOLD)
constexpr float kStickDeadZone = 7849.0f;
constexpr float kTriggerThreshold = 30.0f;

// the buttons RB3's menus read from an instrument
constexpr uint16_t kMenuButtons = xbox::kButtonA | xbox::kButtonB | xbox::kButtonX |
                                  xbox::kButtonY | xbox::kStart | xbox::kBack | xbox::kDpadUp |
                                  xbox::kDpadDown | xbox::kDpadLeft | xbox::kDpadRight;

float Farthest(float a, float b) { return std::abs(b) > std::abs(a) ? b : a; }

}

bool IsStandIn(DeviceKind kind, std::string_view name) {
    if (kind != DeviceKind::kSynthetic) return false;
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("keyboard") == std::string::npos && lower.find("mouse") == std::string::npos;
}

std::string DeviceKindLabel(DeviceKind kind, std::string_view name,
                            std::optional<uint8_t> sub_type) {
    const char* instrument = sub_type ? InstrumentName(*sub_type) : nullptr;
    switch (kind) {
    case DeviceKind::kPad:
        return instrument ? std::string("Xbox 360 ") + instrument : "Controller";
    case DeviceKind::kSynthetic:
        return IsStandIn(kind, name) ? "Stand-in (presses nothing)" : "Keyboard and mouse";
    case DeviceKind::kVirtual:
        return std::string("Virtual ") + (instrument ? instrument : "instrument") + " (debug)";
    case DeviceKind::kHidInstrument:
        return Capitalized(instrument ? instrument : "instrument") + " (USB dongle)";
    case DeviceKind::kMidiDrums: return "MIDI drum kit";
    case DeviceKind::kSdlCopy: return "Dongle instrument as SDL sees it (unused)";
    }
    return "Controller";
}

std::string PlayerLabel(int player) {
    return player > 0 ? "Player " + std::to_string(player) : "Not playing";
}

std::optional<std::string> PlaysAsLabel(int controller_type) {
    switch (controller_type) {
    case -1: return std::nullopt;
    case 1: return "Vocals";
    case 7: return "Guitar";
    case 8: return "Drums";
    default: return "instrument type " + std::to_string(controller_type);
    }
}

TestView ViewFor(const input::Caps360& caps) {
    switch (caps.sub_type) {
    case input::kSubtypeGuitar:
    case input::kSubtypeGuitarAlternate:
    case input::kSubtypeGuitarBass:
        return TestView::kGuitar;
    case input::kSubtypeDrums: return TestView::kDrums;
    default: return TestView::kPad;
    }
}

float FlashLevel(uint8_t velocity, float seconds) {
    if (velocity == 0 || seconds >= kFlashSeconds) return 0.0f;
    const float strength = 0.35f + 0.65f * static_cast<float>(std::min<uint8_t>(velocity, 127) - 1) / 126.0f;
    return strength * (1.0f - std::max(seconds, 0.0f) / kFlashSeconds);
}

float StickAmount(int16_t axis) {
    const float v = static_cast<float>(axis);
    const float past = std::abs(v) - kStickDeadZone;
    if (past <= 0) return 0.0f;
    const float amount = std::min(past / (32767.0f - kStickDeadZone), 1.0f);
    return v < 0 ? -amount : amount;
}

float TriggerAmount(uint8_t trigger) {
    const float past = static_cast<float>(trigger) - kTriggerThreshold;
    return past <= 0 ? 0.0f : past / (255.0f - kTriggerThreshold);
}

bool DrivesNavigation(DeviceKind kind) {
    return kind != DeviceKind::kSynthetic && kind != DeviceKind::kSdlCopy;
}

NavPad NavFromReading(const input::Caps360& caps, const input::Gamepad360& state) {
    NavPad pad;
    if (caps.sub_type == input::kSubtypeGamepad) {
        pad.buttons = state.buttons;
        pad.left_x = StickAmount(state.thumb_lx);
        pad.left_y = StickAmount(state.thumb_ly);
        pad.right_x = StickAmount(state.thumb_rx);
        pad.right_y = StickAmount(state.thumb_ry);
        pad.left_trigger = TriggerAmount(state.left_trigger);
        pad.right_trigger = TriggerAmount(state.right_trigger);
        return pad;
    }
    pad.buttons = state.buttons & kMenuButtons;
    // the cymbal flag (RB) with a cymbal's d-pad marker
    if (caps.sub_type == input::kSubtypeDrums && input::IsRb2Drums(caps) &&
        (state.buttons & xbox::kRightShoulder) != 0) {
        pad.buttons &= static_cast<uint16_t>(~(xbox::kDpadUp | xbox::kDpadDown));
    }
    return pad;
}

NavPad CombinePads(std::span<const NavPad> pads) {
    NavPad all;
    for (const NavPad& p : pads) {
        all.buttons |= p.buttons;
        all.left_x = Farthest(all.left_x, p.left_x);
        all.left_y = Farthest(all.left_y, p.left_y);
        all.right_x = Farthest(all.right_x, p.right_x);
        all.right_y = Farthest(all.right_y, p.right_y);
        all.left_trigger = std::max(all.left_trigger, p.left_trigger);
        all.right_trigger = std::max(all.right_trigger, p.right_trigger);
    }
    return all;
}

NavEdges PressedEdges(uint16_t before, uint16_t now) {
    const uint16_t pressed = static_cast<uint16_t>(now & ~before);
    return {
        .start = (pressed & xbox::kStart) != 0,
        .tab_previous = (pressed & xbox::kLeftShoulder) != 0,
        .tab_next = (pressed & xbox::kRightShoulder) != 0,
    };
}

void TestMode::Begin(uint64_t id) {
    if (testing_ == id) return;
    End();
    testing_ = id;
    last_ = 0;
    primed_ = false;
    if (ended_ == id) ended_.reset();
}

void TestMode::End() {
    if (!testing_) return;
    ended_ = testing_;
    ended_held_ = primed_ ? last_ : 0;
    testing_.reset();
}

void TestMode::Follow(std::span<const uint64_t> connected, bool showing) {
    if (testing_ && (!showing || std::ranges::find(connected, *testing_) == connected.end())) {
        End();
    }
}

NavPad TestMode::Filter(uint64_t id, const NavPad& pad) {
    if (testing_ == id) {
        // a Back held as the test began isn't a press
        const bool back = primed_ && (pad.buttons & ~last_ & xbox::kBack) != 0;
        last_ = pad.buttons;
        primed_ = true;
        if (back) End();
        return {};
    }
    if (ended_ != id) return pad;
    ended_held_ &= pad.buttons;
    NavPad free = pad;
    free.buttons &= static_cast<uint16_t>(~ended_held_);
    if (ended_held_ == 0) ended_.reset();
    return free;
}

}
