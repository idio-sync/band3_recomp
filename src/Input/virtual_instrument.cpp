#include "virtual_instrument.h"
#include <algorithm>
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

const char* InstrumentKindId(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return "guitar";
    case InstrumentKind::kDrums: return "drums";
    case InstrumentKind::kKeys: return "keys";
    case InstrumentKind::kProGuitarMustang: return "pro_guitar_mustang";
    case InstrumentKind::kProGuitarSquier: return "pro_guitar_squier";
    }
    return "guitar";
}

std::optional<InstrumentKind> ParseInstrumentKind(std::string_view id) {
    for (InstrumentKind kind : kInstrumentKinds) {
        if (id == InstrumentKindId(kind)) return kind;
    }
    return std::nullopt;
}

const char* InstrumentKindLabel(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return "Guitar";
    case InstrumentKind::kDrums: return "Drums";
    case InstrumentKind::kKeys: return "Keys";
    case InstrumentKind::kProGuitarMustang: return "Pro Guitar (Mustang)";
    case InstrumentKind::kProGuitarSquier: return "Pro Guitar (Squier)";
    }
    return "Guitar";
}

Caps360 CapsFor(InstrumentKind kind) {
    switch (kind) {
    case InstrumentKind::kGuitar: return GuitarCaps();
    case InstrumentKind::kDrums: return DrumCaps();
    case InstrumentKind::kKeys: return KeysCaps();
    case InstrumentKind::kProGuitarMustang: return ProGuitarCaps(ProGuitarModel::kMustang);
    case InstrumentKind::kProGuitarSquier: return ProGuitarCaps(ProGuitarModel::kSquier);
    }
    return GuitarCaps();
}

Gamepad360 Encode(InstrumentKind kind, const InstrumentInputs& in) {
    switch (kind) {
    case InstrumentKind::kGuitar: return EncodeGuitar(in.guitar);
    case InstrumentKind::kDrums: return EncodeDrums(in.drums);
    case InstrumentKind::kKeys: return EncodeKeys(in.keys);
    case InstrumentKind::kProGuitarMustang:
    case InstrumentKind::kProGuitarSquier: return EncodeProGuitar(in.pro_guitar);
    }
    return {};
}

// VirtualInstrument

VirtualInstrument& VirtualInstrument::Get() {
    static VirtualInstrument instance;
    return instance;
}

void VirtualInstrument::SetKind(InstrumentKind kind) {
    // the change callback updates kind_
    rex::cvar::SetFlagByName("virtual_instrument_type", InstrumentKindId(kind));
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
    auto& instrument = VirtualInstrument::Get();
    auto apply = [&instrument](std::string_view id) {
        if (auto kind = ParseInstrumentKind(id)) instrument.kind_.store(*kind);
    };
    apply(REXCVAR_GET(virtual_instrument_type));
    rex::cvar::RegisterChangeCallback("virtual_instrument_type",
        [apply](std::string_view, std::string_view value) { apply(value); });
}

namespace {

// identifies the virtual instrument to the player assignment
constexpr const char* kVirtualGuid = "band3-virtual-instrument";
// clear of the SDK drivers' ids (SDL counts up from 1, MnK and NOP use 0x4D4E4B00
// and 0x4E4F5000)
constexpr uint64_t kDeviceIdBase = 0x4233564900000000ull;  // "B3VI"
// how long a type or player change leaves it unplugged, so RB3 sees it go and
// reads the new capabilities when it comes back
constexpr std::chrono::milliseconds kReplugDelay{500};

class VirtualInstrumentDriver final : public rex::input::InputDriver {
public:
    VirtualInstrumentDriver() : InputDriver(nullptr, 0) {}

    X_STATUS Setup() override { return X_STATUS_SUCCESS; }

    void EnumerateDevices(std::vector<DeviceInfo>& out) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!REXCVAR_GET(virtual_instrument)) {
            id_ = DeviceId::kInvalid;
            return;
        }

        const InstrumentKind kind = VirtualInstrument::Get().kind();
        const int32_t player = REXCVAR_GET(virtual_instrument_player);
        const auto now = std::chrono::steady_clock::now();
        if (id_ != DeviceId::kInvalid && (kind != kind_ || player != player_)) {
            id_ = DeviceId::kInvalid;
            replug_at_ = now + kReplugDelay;
        }
        if (id_ == DeviceId::kInvalid) {
            if (now < replug_at_) return;
            kind_ = kind;
            player_ = player;
            // a new id reads as a new controller, so the SDK reassigns it
            id_ = static_cast<DeviceId>(kDeviceIdBase + ++generation_);
            REXLOG_INFO("Virtual instrument connected: {} on player {}",
                        InstrumentKindLabel(kind_), player_);
        }

        DeviceInfo info;
        info.id = id_;
        info.name = std::string("band3 virtual ") + InstrumentKindLabel(kind_);
        info.guid = kVirtualGuid;
        // RB3 reads the subtype from GetDeviceCapabilities; DeviceInfo only has
        // a subtype field in some SDK builds, so it is left to them
        // synthetic devices never take a physical ordinal, so real pads keep
        // theirs while it comes and goes; PlayerAssignment places it by its guid
        info.synthetic = true;
        out.push_back(std::move(info));
    }

    X_RESULT GetDeviceState(DeviceId id, rex::input::X_INPUT_STATE* out_state) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == DeviceId::kInvalid || id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;

        const Gamepad360 g = Encode(kind_, VirtualInstrument::Get().Current());
        if (std::memcmp(&g, &last_, sizeof(g)) != 0) {
            last_ = g;
            packet_number_++;
        }
        if (out_state) {
            out_state->packet_number = packet_number_;
            StoreGamepad(g, out_state->gamepad);
        }
        return X_ERROR_SUCCESS;
    }

    X_RESULT GetDeviceCapabilities(DeviceId id, uint32_t,
                                   rex::input::X_INPUT_CAPABILITIES* out_caps) override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (id == DeviceId::kInvalid || id != id_) return X_ERROR_DEVICE_NOT_CONNECTED;
        if (out_caps) StoreCaps(CapsFor(kind_), *out_caps);
        return X_ERROR_SUCCESS;
    }

    X_RESULT SetDeviceVibration(DeviceId id, rex::input::X_INPUT_VIBRATION*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return id != DeviceId::kInvalid && id == id_ ? X_ERROR_SUCCESS
                                                     : X_ERROR_DEVICE_NOT_CONNECTED;
    }

    X_RESULT GetDeviceKeystroke(DeviceId id, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
        std::lock_guard<std::mutex> lock(mutex_);
        return id != DeviceId::kInvalid && id == id_ ? X_ERROR_EMPTY
                                                     : X_ERROR_DEVICE_NOT_CONNECTED;
    }

private:
    // RefreshDevices runs on whichever guest thread polls
    std::mutex mutex_;
    DeviceId id_ = DeviceId::kInvalid;
    uint64_t generation_ = 0;
    InstrumentKind kind_ = InstrumentKind::kGuitar;
    int32_t player_ = 0;
    std::chrono::steady_clock::time_point replug_at_{};
    Gamepad360 last_{};
    uint32_t packet_number_ = 0;
};

}

std::unique_ptr<rex::input::InputDriver> CreateVirtualInstrumentDriver() {
    return std::make_unique<VirtualInstrumentDriver>();
}

bool IsVirtualInstrument(const rex::input::DeviceInfo& device) {
    return device.guid == kVirtualGuid;
}

}
