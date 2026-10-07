#include "menu_shortcut.h"
#include "instruments.h"

namespace band3::input {

namespace {

constexpr uint16_t kChordButtons = xbox::kLeftThumb | xbox::kRightThumb | xbox::kLeftShoulder;

// the other buttons don't matter; the chord's own must match exactly, so adding
// the bumper to a held pause menu chord switches to the Instrument Lab one
MenuShortcutAction ChordFor(uint16_t buttons) {
    switch (buttons & kChordButtons) {
    case xbox::kLeftThumb | xbox::kRightThumb:
        return MenuShortcutAction::kPauseMenu;
    case xbox::kLeftThumb | xbox::kRightThumb | xbox::kLeftShoulder:
        return MenuShortcutAction::kInstrumentLab;
    default:
        return MenuShortcutAction::kNone;
    }
}

}

MenuShortcutAction MenuShortcut::Update(uint16_t buttons, Clock::time_point now) {
    if ((buttons & kChordButtons) == 0) fired_ = false;

    const MenuShortcutAction chord = ChordFor(buttons);
    if (chord != held_) {
        held_ = chord;
        held_since_ = now;
    }
    if (held_ == MenuShortcutAction::kNone || fired_ || now - held_since_ < kHoldTime) {
        return MenuShortcutAction::kNone;
    }
    fired_ = true;
    return held_;
}

void ChordPads::OnCapabilities(uint32_t player, uint8_t subtype) {
    if (player >= players_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    players_[player].subtype = subtype;
}

void ChordPads::OnState(uint32_t player, std::optional<uint16_t> buttons, Clock::time_point now) {
    if (player >= players_.size()) return;
    std::lock_guard<std::mutex> lock(mutex_);
    players_[player].buttons = buttons.value_or(0);
    players_[player].read = now;
}

uint16_t ChordPads::Held(Clock::time_point now) const {
    std::lock_guard<std::mutex> lock(mutex_);
    uint16_t held = 0;
    for (const Player& p : players_) {
        if (!p.subtype || IsRb3InstrumentSubtype(*p.subtype)) continue;
        if (now - p.read > kStale) continue;
        held |= p.buttons;
    }
    return held;
}

ChordPads& GameChordPads() {
    static ChordPads pads;
    return pads;
}

}
