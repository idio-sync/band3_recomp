#include "virtual_instrument.h"
#include <algorithm>
#include <charconv>
#include <cstring>
#include <string>
#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include "src/settings.h"
#include "xinput_state.h"

namespace band3::input {

using rex::X_RESULT;
using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;

namespace {

// the player the settings' instrument is on, once InitVirtualInstrument has run
std::atomic<int> g_settings_player{0};

int ClampPlayer(int player) { return std::clamp(player, 1, kPlayers); }

}

// VirtualInstrument

// Moving to another player takes the instrument out of the old one, as
// unplugging a pad and plugging it in again.
void VirtualInstrument::ApplySettings(bool enabled, std::string_view type, int player) {
    player = ClampPlayer(player);
    const int old = g_settings_player.exchange(player);
    if (old != 0 && old != player) ForPlayer(old).plugged_.store(false);

    auto& instrument = ForPlayer(player);
    if (auto kind = ParseInstrumentKind(type)) instrument.kind_.store(*kind);
    instrument.plugged_.store(enabled);
}

VirtualInstrument& VirtualInstrument::ForPlayer(int player) {
    static VirtualInstrument* const instruments = [] {
        static VirtualInstrument all[kPlayers];
        for (int p = 0; p < kPlayers; p++) all[p].player_ = p + 1;
        return all;
    }();
    return instruments[ClampPlayer(player) - 1];
}

VirtualInstrument& VirtualInstrument::FromSettings() {
    const int player = g_settings_player.load();
    return ForPlayer(player != 0 ? player : REXCVAR_GET(virtual_instrument_player));
}

bool VirtualInstrument::FromTheSettings() const {
    return player_ == g_settings_player.load();
}

// The settings' instrument changes through the settings, whose callbacks apply
// the change (ApplySettings); it is applied here too, which is the same change,
// so a setting that already had the value changes nothing.
void VirtualInstrument::Plug(InstrumentKind kind) {
    if (FromTheSettings()) {
        rex::cvar::SetFlagByName("virtual_instrument_type", InstrumentKindId(kind));
        rex::cvar::SetFlagByName("virtual_instrument", "true");
    }
    kind_.store(kind);
    plugged_.store(true);
}

void VirtualInstrument::Unplug() {
    if (FromTheSettings()) rex::cvar::SetFlagByName("virtual_instrument", "false");
    plugged_.store(false);
}

void VirtualInstrument::SetKind(InstrumentKind kind) {
    if (FromTheSettings()) {
        rex::cvar::SetFlagByName("virtual_instrument_type", InstrumentKindId(kind));
    }
    kind_.store(kind);
}

InstrumentInputs VirtualInstrument::Held() {
    std::lock_guard<std::mutex> lock(mutex_);
    return held_;
}

void VirtualInstrument::SetHeld(const InstrumentInputs& in) {
    std::lock_guard<std::mutex> lock(mutex_);
    held_ = in;
}

void VirtualInstrument::Pulse(std::function<void(InstrumentInputs&)> change,
                              std::chrono::milliseconds length) {
    std::lock_guard<std::mutex> lock(mutex_);
    pulses_.emplace_back(Clock::now() + length, std::move(change));
}

InstrumentInputs VirtualInstrument::Current() {
    std::lock_guard<std::mutex> lock(mutex_);
    auto now = Clock::now();
    std::erase_if(pulses_, [&](const auto& pulse) { return pulse.first <= now; });
    InstrumentInputs in = held_;
    for (const auto& pulse : pulses_) {
        pulse.second(in);
    }
    return in;
}

void InitVirtualInstrument() {
    VirtualInstrument::ApplySettings(REXCVAR_GET(virtual_instrument),
                                     REXCVAR_GET(virtual_instrument_type),
                                     REXCVAR_GET(virtual_instrument_player));
    // the changed setting's new value comes with the callback; the others are read
    auto on_change = [](std::string_view name, std::string_view value) {
        bool enabled = REXCVAR_GET(virtual_instrument);
        std::string type = REXCVAR_GET(virtual_instrument_type);
        int player = REXCVAR_GET(virtual_instrument_player);
        if (name == "virtual_instrument") {
            enabled = value == "true" || value == "1";
        } else if (name == "virtual_instrument_type") {
            type = value;
        } else {
            std::from_chars(value.data(), value.data() + value.size(), player);
        }
        VirtualInstrument::ApplySettings(enabled, type, player);
    };
    for (const char* name : {"virtual_instrument", "virtual_instrument_type",
                             "virtual_instrument_player"}) {
        rex::cvar::RegisterChangeCallback(name, on_change);
    }
}

std::array<bool, kPlayers> VirtualInstrumentPlayers() {
    std::array<bool, kPlayers> players{};
    for (int p = 0; p < kPlayers; p++) players[p] = VirtualInstrument::ForPlayer(p + 1).plugged();
    return players;
}

namespace {

// identifies a virtual instrument and its player to the player assignment
constexpr std::string_view kVirtualGuid = "band3-virtual-instrument-";
// clear of the SDK drivers' ids (SDL counts up from 1, MnK and NOP use 0x4D4E4B00
// and 0x4E4F5000)
constexpr uint64_t kDeviceIdBase = 0x4233564900000000ull;  // "B3VI"
// how long a type change leaves it unplugged, so RB3 sees it go and reads the
// new capabilities when it comes back
constexpr std::chrono::milliseconds kReplugDelay{500};

class VirtualInstrumentDriver final : public rex::input::InputDriver {
public:
    VirtualInstrumentDriver() : InputDriver(nullptr, 0) {}

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = std::chrono::steady_clock::now();
        for (int p = 0; p < kPlayers; p++) {
            Slot& slot = slots_[p];
            const auto& instrument = VirtualInstrument::ForPlayer(p + 1);
            if (!instrument.plugged()) {
                slot.id = DeviceId::kInvalid;
                continue;
            }
            const InstrumentKind kind = instrument.kind();
            if (slot.id != DeviceId::kInvalid && kind != slot.kind) {
                slot.id = DeviceId::kInvalid;
                slot.replug_at = now + kReplugDelay;
            }
            if (slot.id == DeviceId::kInvalid) {
                if (now < slot.replug_at) continue;
                slot.kind = kind;
                // a new id reads as a new controller, so the SDK reassigns it
                slot.id = static_cast<DeviceId>(kDeviceIdBase + (uint64_t(p + 1) << 24) +
                                                ++slot.generation);
                REXLOG_INFO("Virtual instrument connected: {} on player {}",
                            InstrumentKindLabel(slot.kind), p + 1);
            }

            DeviceInfo info;
            info.id = slot.id;
            info.name = std::string("band3 virtual ") + InstrumentKindLabel(slot.kind);
            info.guid = std::string(kVirtualGuid) + std::to_string(p + 1);
            // RB3 reads the subtype from GetDeviceCapabilities; DeviceInfo only has
            // a subtype field in some SDK builds, so it is left to them
            // synthetic devices never take a physical ordinal, so real pads keep
            // theirs while it comes and goes; PlayerAssignment places it by its guid
            info.synthetic = true;
            out.push_back(std::move(info));
        }
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int p = SlotOf(id);
        if (p < 0) return X_ERROR_DEVICE_NOT_CONNECTED;

        Slot& slot = slots_[p];
        const Gamepad360 g = Encode(slot.kind, VirtualInstrument::ForPlayer(p + 1).Current());
        if (std::memcmp(&g, &slot.last, sizeof(g)) != 0) {
            slot.last = g;
            slot.packet_number++;
        }
        if (out_state) {
            out_state->packet_number = slot.packet_number;
            StoreGamepad(g, out_state->gamepad);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                   rex::input::X_INPUT_CAPABILITIES* out_caps) override {
        std::lock_guard<std::mutex> lock(mutex_);
        const int p = SlotOf(id);
        if (p < 0) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out_caps) StoreCaps(CapsFor(slots_[p].kind), *out_caps);
        return X_ERROR_SUCCESS;
    }

    X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return SlotOf(id) >= 0 ? X_ERROR_SUCCESS : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return SlotOf(id) >= 0 ? X_ERROR_EMPTY : X_ERROR_DEVICE_NOT_CONNECTED;
    }

private:
    struct Slot {
        DeviceId id = DeviceId::kInvalid;
        uint64_t generation = 0;
        InstrumentKind kind = InstrumentKind::kGuitar;
        std::chrono::steady_clock::time_point replug_at{};
        Gamepad360 last{};
        uint32_t packet_number = 0;
    };

    // the slot a connected device is, or -1
    int SlotOf(DeviceId id) const {
        if (id == DeviceId::kInvalid) return -1;
        for (int p = 0; p < kPlayers; p++) {
            if (slots_[p].id == id) return p;
        }
        return -1;
    }

    // RefreshDevices runs on whichever guest thread polls
    std::mutex mutex_;
    std::array<Slot, kPlayers> slots_{};
};

}

std::unique_ptr<rex::input::InputDriver> CreateVirtualInstrumentDriver() {
    return std::make_unique<VirtualInstrumentDriver>();
}

int VirtualInstrumentPlayer(const rex::input::DeviceInfo& device) {
    if (!device.guid.starts_with(kVirtualGuid)) return 0;
    int player = 0;
    const std::string_view digits = std::string_view(device.guid).substr(kVirtualGuid.size());
    std::from_chars(digits.data(), digits.data() + digits.size(), player);
    return player >= 1 && player <= kPlayers ? player : 0;
}

}
