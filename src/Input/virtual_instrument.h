#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>
#include <rex/input/input_driver.h>
#include "instrument_kind.h"

// A virtual Xbox 360 instrument that the Instrument Lab (F6) plays, for checking
// how RB3 reads each instrument without the hardware. It shows up as its own
// controller, on the player slot virtual_instrument_player, while the
// virtual_instrument setting is on.

namespace band3::input {

// What the virtual instrument is pressing. The Lab changes it on the UI thread
// and the driver reads it on guest threads.
class VirtualInstrument {
public:
    // long enough for RB3 to see a hit on at least a couple of polls
    static constexpr std::chrono::milliseconds kPulseLength{60};

    static VirtualInstrument& Get();

    InstrumentKind kind() const { return kind_.load(); }
    // sets virtual_instrument_type, which unplugs and replugs the instrument
    void SetKind(InstrumentKind kind);

    // what is held down, without pulses
    InstrumentInputs Held();
    void SetHeld(const InstrumentInputs& in);

    // applies a change for a moment, like a drum hit or a plucked string
    void Pulse(std::function<void(InstrumentInputs&)> change,
               std::chrono::milliseconds length = kPulseLength);

    // what is held plus the pulses still running
    InstrumentInputs Current();

private:
    friend void InitVirtualInstrument();

    using Clock = std::chrono::steady_clock;

    std::mutex mutex_;
    InstrumentInputs held_;
    std::vector<std::pair<Clock::time_point, std::function<void(InstrumentInputs&)>>> pulses_;
    std::atomic<InstrumentKind> kind_{InstrumentKind::kGuitar};
};

// Follows virtual_instrument_type from here on. Call once, after the settings load.
void InitVirtualInstrument();

// the driver that connects the virtual instrument while virtual_instrument is on
std::unique_ptr<rex::input::InputDriver> CreateVirtualInstrumentDriver();
bool IsVirtualInstrument(const rex::input::DeviceInfo& device);

}
