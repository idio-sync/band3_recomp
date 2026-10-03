#pragma once
#include <chrono>
#include <optional>

// When to restart a driver for settings that changed: once they have stayed the
// same for a moment, rather than on every change. A dropdown clicked through,
// or a value changed several times in a row, then restarts the driver once,
// not once a frame (a restart closes and reopens its device, which leaves the
// device list and reconnects as a new controller). Pure, so the unit tests can
// check it; time is passed in.

namespace band3::input {

template <typename Config>
class RestartDebounce {
public:
    using Clock = std::chrono::steady_clock;

    explicit RestartDebounce(std::chrono::milliseconds settle) : settle_(settle) {}

    // Whether to restart now for `wanted`, the settings as they are at `now`,
    // with `running` those the driver started with: once `wanted` differs from
    // `running` and hasn't changed for the settle time. Once a frame.
    bool Due(const Config& wanted, const Config& running, Clock::time_point now) {
        if (wanted == running) {
            pending_.reset();
            return false;
        }
        if (!pending_ || !(*pending_ == wanted)) {
            pending_ = wanted;
            since_ = now;
        }
        return now - since_ >= settle_;
    }

private:
    std::chrono::milliseconds settle_;
    // the settings waiting to settle, and since when they have been as they are
    std::optional<Config> pending_;
    Clock::time_point since_{};
};

}
