#include "mouse_menus.h"
#include <algorithm>
#include <cstdlib>

namespace band3::input {

void MouseMenus::Key::Queue() { queued = std::min(queued + 1, kMaxQueued); }

bool MouseMenus::Key::Step(Clock::time_point now) {
    if (on) {
        // a held button keeps its press on only while it's the newest press
        if (now < on_until || (held && queued == 0)) return true;
        on = false;
        off_until = now + kPulse;
        return false;
    }
    if (queued == 0 || now < off_until) return false;
    queued--;
    on = true;
    on_until = now + kPulse;
    return true;
}

void MouseMenus::Press(Button button) {
    Key& key = KeyFor(button);
    key.held = true;
    key.Queue();
}

void MouseMenus::Release(Button button) { KeyFor(button).held = false; }

void MouseMenus::Scroll(int32_t amount, int32_t per_notch) {
    if (per_notch <= 0 || amount == 0) return;
    // turning the wheel back starts afresh, rather than first undoing the part
    // of a notch the other way
    if ((scrolled_ > 0) != (amount > 0)) scrolled_ = 0;
    scrolled_ += amount;
    const int32_t notches = scrolled_ / per_notch;
    scrolled_ -= notches * per_notch;
    if (notches == 0) return;
    Key& toward = notches > 0 ? keys_[2] : keys_[3];
    Key& away = notches > 0 ? keys_[3] : keys_[2];
    away.queued = 0;
    // and never presses up and down at once
    if (away.on) toward.off_until = std::max(toward.off_until, away.on_until + kPulse);
    for (int32_t i = 0; i < std::min(std::abs(notches), kMaxQueued); i++) toward.Queue();
}

void MouseMenus::Clear() {
    for (Key& key : keys_) key = Key{.bit = key.bit};
    scrolled_ = 0;
}

uint16_t MouseMenus::Buttons(Clock::time_point now) {
    uint16_t buttons = 0;
    for (Key& key : keys_) {
        if (key.Step(now)) buttons |= key.bit;
    }
    return buttons;
}

}
