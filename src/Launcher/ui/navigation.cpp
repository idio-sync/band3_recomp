#include "navigation.h"
#include <cstdlib>
#include "src/Input/instruments.h"

namespace band3::launcher::ui {

namespace xbox = input::xbox;

namespace {

// a stick past this counts as a direction held
constexpr float kStickPress = 0.5f;

}

int MoveSelection(int index, int delta, int count, bool wrap,
                  const std::function<bool(int)>& focusable) {
    if (count <= 0) return -1;
    const auto ok = [&](int i) { return !focusable || focusable(i); };
    const int step = delta > 0 ? 1 : -1;
    for (int moves = std::abs(delta); moves > 0; moves--) {
        int next = index;
        bool found = false;
        for (int tries = 0; tries < count; tries++) {
            next += step;
            if (wrap) {
                next = (next % count + count) % count;
            } else if (next < 0 || next >= count) {
                break;
            }
            if (next == index) break;
            if (ok(next)) {
                found = true;
                break;
            }
        }
        if (!found) break;
        index = next;
    }
    return index;
}

int FirstFocusable(int count, const std::function<bool(int)>& focusable) {
    for (int i = 0; i < count; i++) {
        if (!focusable || focusable(i)) return i;
    }
    return -1;
}

bool RepeatGate::Step(bool held, double now) {
    if (!held) {
        held_ = false;
        return false;
    }
    if (!held_) {
        held_ = true;
        next_ = now + kDelay;
        return true;
    }
    if (now < next_) return false;
    next_ += kInterval;
    // after a long frame, from now rather than in a burst
    if (next_ < now) next_ = now + kInterval;
    return true;
}

bool HoldGate::Step(bool held, double now) {
    if (!held) {
        held_ = false;
        fired_ = false;
        return false;
    }
    if (!held_) {
        held_ = true;
        fired_ = false;
        since_ = now;
        return false;
    }
    if (fired_ || now - since_ < kDuration) return false;
    fired_ = true;
    return true;
}

double HoldGate::Progress(double now) const {
    if (!held_) return 0;
    if (fired_) return 1;
    const double p = (now - since_) / kDuration;
    return p < 0 ? 0 : p > 1 ? 1 : p;
}

std::vector<Action> PadActions::Step(const NavPad& pad, double now) {
    const uint16_t buttons = pad.buttons;
    const bool held[4] = {
        (buttons & xbox::kDpadUp) || pad.left_y > kStickPress,
        (buttons & xbox::kDpadDown) || pad.left_y < -kStickPress,
        (buttons & xbox::kDpadLeft) || pad.left_x < -kStickPress,
        (buttons & xbox::kDpadRight) || pad.left_x > kStickPress,
    };
    RepeatGate* gates[4] = {&up_, &down_, &left_, &right_};
    constexpr Action kDirections[4] = {Action::kUp, Action::kDown, Action::kLeft, Action::kRight};

    std::vector<Action> out;
    if (!primed_) {
        // what's held now counts only once let go and pressed again
        primed_ = true;
        last_buttons_ = buttons;
        for (int i = 0; i < 4; i++) blocked_[i] = held[i];
        start_hold_.Step(false, now);
        held_start_ = false;
        return out;
    }
    for (int i = 0; i < 4; i++) {
        if (blocked_[i]) {
            if (held[i]) continue;
            blocked_[i] = false;
        }
        if (gates[i]->Step(held[i], now)) out.push_back(kDirections[i]);
    }
    const uint16_t pressed = buttons & ~last_buttons_;
    last_buttons_ = buttons;
    if (pressed & xbox::kButtonA) out.push_back(Action::kSelect);
    if (pressed & (xbox::kButtonB | xbox::kBack)) out.push_back(Action::kBack);
    if (pressed & xbox::kButtonX) out.push_back(Action::kSecondary);
    if (pressed & xbox::kButtonY) out.push_back(Action::kReset);
    if (pressed & xbox::kLeftShoulder) out.push_back(Action::kPreviousCategory);
    if (pressed & xbox::kRightShoulder) out.push_back(Action::kNextCategory);
    if (pressed & xbox::kStart) out.push_back(Action::kPlay);
    held_start_ = start_hold_.Step((buttons & xbox::kStart) != 0, now);
    return out;
}

}
