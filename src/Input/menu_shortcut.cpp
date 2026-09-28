#include "menu_shortcut.h"
#include "instruments.h"

namespace band3::input {

namespace {

constexpr uint16_t kChordButtons = xbox::kLeftThumb | xbox::kRightThumb | xbox::kLeftShoulder;

// the other buttons don't matter; the chord's own must match exactly, so adding
// the bumper to a held settings chord switches to the Instrument Lab one
MenuShortcutAction ChordFor(uint16_t buttons) {
    switch (buttons & kChordButtons) {
    case xbox::kLeftThumb | xbox::kRightThumb:
        return MenuShortcutAction::kSettings;
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

}
