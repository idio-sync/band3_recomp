#pragma once
#include <cstdint>
#include <functional>
#include <vector>
#include "src/Launcher/device_view_model.h"

// The launcher's own navigation (the redesign drops ImGui's): one set of
// actions every screen understands, whether they come from a controller, the
// keyboard or the mouse, and the pieces that turn held buttons into them.

namespace band3::launcher::ui {

enum class Action : uint8_t {
    kUp,
    kDown,
    kLeft,
    kRight,
    kSelect,     // A, Enter
    kBack,       // B, Esc
    kSecondary,  // X, /
    kReset,      // Y, R
    kPreviousCategory,  // LB, Shift+Tab
    kNextCategory,      // RB, Tab
    kPlay,              // Start, F5
};

// The selection after moving `delta` items through `count`, skipping those
// `focusable` refuses: wrapping past the ends when `wrap`, otherwise stopping
// at the last focusable one. `index` back when nothing else is focusable, -1
// when nothing is.
int MoveSelection(int index, int delta, int count, bool wrap,
                  const std::function<bool(int)>& focusable = nullptr);

// the first item `focusable` allows, -1 for none
int FirstFocusable(int count, const std::function<bool(int)>& focusable = nullptr);

// A held direction: it fires as it's pressed, again after kDelay, then every
// kInterval while it stays held.
class RepeatGate {
public:
    static constexpr double kDelay = 0.38;
    static constexpr double kInterval = 0.07;
    // whether it fires this frame, at `now` seconds
    bool Step(bool held, double now);

private:
    bool held_ = false;
    double next_ = 0;
};

// A button held long enough: fires once, when it has been held kDuration.
class HoldGate {
public:
    static constexpr double kDuration = 1.0;
    bool Step(bool held, double now);
    // 0 to 1, how far through the hold it is (0 when not held)
    double Progress(double now) const;

private:
    bool held_ = false;
    bool fired_ = false;
    double since_ = 0;
};

// A controller's buttons and left stick (NavPad, XInput's layout) as actions:
// the face buttons, bumpers and Start as they're pressed, the D-pad and the
// stick as RepeatGate repeats them. Buttons already held at the first step
// count only once let go and pressed again.
class PadActions {
public:
    std::vector<Action> Step(const NavPad& pad, double now);
    // a held Start (stops a controller's test)
    bool HeldStart() const { return held_start_; }

private:
    bool primed_ = false;
    uint16_t last_buttons_ = 0;
    // directions held at the first step, ignored until let go
    bool blocked_[4] = {};
    RepeatGate up_, down_, left_, right_;
    HoldGate start_hold_;
    bool held_start_ = false;
};

}
