#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <utility>
#include <vector>
#include <rex/input/input_driver.h>
#include "instrument_kind.h"
#include "player_slots.h"

// Virtual Xbox 360 instruments, one per player, for checking how RB3 reads each
// instrument without the hardware. Each plugged-in one shows up as its own
// controller on its player.
//
// The settings describe one of them, the Instrument Lab's (F6): virtual_instrument
// plugs it into player virtual_instrument_player as virtual_instrument_type. The
// test harness plugs in the others (players it isn't using) directly.

namespace band3::input {

// What a virtual instrument is pressing. The Lab and the test harness change it
// on their threads and the driver reads it on guest threads.
class VirtualInstrument {
public:
    // long enough for RB3 to see a hit on at least a couple of polls
    static constexpr std::chrono::milliseconds kPulseLength{60};

    // player 1-4's
    static VirtualInstrument& ForPlayer(int player);
    // the one the settings describe, on virtual_instrument_player: the Lab's
    static VirtualInstrument& FromSettings();

    int player() const { return player_; }
    bool plugged() const { return plugged_.load(); }
    InstrumentKind kind() const { return kind_.load(); }

    // Plugs it in as `kind`, or replugs it as that kind; a kind change unplugs
    // it for a moment. The settings' instrument goes through the settings, so
    // call these on the UI thread.
    void Plug(InstrumentKind kind);
    void Unplug();
    // a new kind for a plugged-in or unplugged instrument alike
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

    bool FromTheSettings() const;
    // the settings' values, on the instruments; never writes the settings, so
    // the change callbacks can call it
    static void ApplySettings(bool enabled, std::string_view type, int player);

    int player_ = 0;
    std::mutex mutex_;
    InstrumentInputs held_;
    std::vector<std::pair<Clock::time_point, std::function<void(InstrumentInputs&)>>> pulses_;
    std::atomic<bool> plugged_{false};
    std::atomic<InstrumentKind> kind_{InstrumentKind::kGuitar};
};

// Follows the virtual_instrument settings from here on. Call once, after the
// settings load.
void InitVirtualInstrument();

// the players kept for virtual instruments: plugged in, or replugging
std::array<bool, kPlayers> VirtualInstrumentPlayers();

// the driver that connects every plugged-in virtual instrument
std::unique_ptr<rex::input::InputDriver> CreateVirtualInstrumentDriver();
// the player a device is the virtual instrument of, or 0
int VirtualInstrumentPlayer(const rex::input::DeviceInfo& device);

}
