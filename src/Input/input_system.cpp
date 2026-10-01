#include "input_system.h"
#include <algorithm>
#include <vector>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include "hid_instruments.h"
#include "midi_drums_driver.h"
#include "src/settings.h"
#include "virtual_instrument.h"

namespace band3::input {

using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

namespace {

// The SDK's SlotAssignment, plus:
// - a fixed slot for the virtual instrument: it feeds virtual_instrument_player
//   and real pads take the other slots in the order they connected. The slot
//   stays reserved while virtual_instrument is on, including the moment a type
//   change leaves the instrument unplugged, so real pads never shift under it.
// - SDL's copy of an instrument the HID driver reads is left out, and the other
//   pads close up over its slot, as if it had never connected.
// The keyboard and other synthetic devices feed player 1, as in the SDK, unless
// the virtual instrument has player 1: then it has the slot to itself, so a type
// change empties the slot and RB3 reads the new type when it comes back (RB3
// only reads a pad's type when its slot connects).
class PlayerAssignment final : public rex::input::DeviceAssignment {
public:
    void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
        for (auto& user : users_) user.clear();

        const bool reserved = REXCVAR_GET(virtual_instrument);
        const uint32_t instrument_user = static_cast<uint32_t>(
            std::clamp<int32_t>(REXCVAR_GET(virtual_instrument_player), 1, kMaxGuestUsers) - 1);

        std::vector<uint32_t> skipped_ordinals;
        for (const auto& device : devices) {
            if (IsSdlCopyOfHidInstrument(device)) skipped_ordinals.push_back(device.ordinal);
        }

        for (const auto& device : devices) {
            if (IsVirtualInstrument(device)) {
                users_[instrument_user].push_back(device.id);
                continue;
            }
            if (IsSdlCopyOfHidInstrument(device)) continue;
            if (device.synthetic) {
                if (!(reserved && instrument_user == 0)) users_[0].push_back(device.id);
                continue;
            }
            uint32_t user = device.ordinal;
            for (uint32_t skipped : skipped_ordinals) {
                if (skipped < device.ordinal) user--;
            }
            if (reserved && user >= instrument_user) user++;
            if (user < kMaxGuestUsers) users_[user].push_back(device.id);
        }
    }

    void DevicesForUser(uint32_t user_index, std::vector<DeviceId>& out) const override {
        out.clear();
        if (user_index < kMaxGuestUsers) out = users_[user_index];
    }

private:
    std::vector<std::vector<DeviceId>> users_ = std::vector<std::vector<DeviceId>>(kMaxGuestUsers);
};

}

namespace {
rex::input::InputSystem* g_game_input = nullptr;
}

std::unique_ptr<rex::system::IInputSystem> CreateInputSystem(bool tool_mode) {
    auto input = rex::input::CreateDefaultInputSystem(tool_mode);
    if (tool_mode) return input;

    auto add = [&input](std::unique_ptr<rex::input::InputDriver> driver) {
        if (driver->Setup() == X_STATUS_SUCCESS) input->AddDriver(std::move(driver));
    };
    add(CreateVirtualInstrumentDriver());
    if (REXCVAR_GET(hid_instruments)) add(CreateHidInstrumentDriver());
    if (REXCVAR_GET(midi_drums)) add(CreateMidiDrumsDriver());

    input->SetDeviceAssignment(std::make_unique<PlayerAssignment>());
    g_game_input = input.get();
    return input;
}

rex::input::InputSystem* GameInputSystem() { return g_game_input; }

}
