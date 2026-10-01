#include "input_system.h"
#include <algorithm>
#include <vector>
#include <rex/input/device_assignment.h>
#include <rex/input/input_system.h>
#include "hid_instruments.h"
#include "midi_drums_driver.h"
#include "player_slots.h"
#include "src/settings.h"
#include "virtual_instrument.h"

namespace band3::input {

using rex::X_STATUS;
using rex::input::DeviceId;
using rex::input::DeviceInfo;
using rex::input::kMaxGuestUsers;

namespace {

// The SDK's SlotAssignment, plus (see player_slots.h):
// - a fixed player for each virtual instrument; real pads take the other players
//   in the order they connected. A player stays reserved while its instrument is
//   plugged in, including the moment a type change leaves it unplugged, so real
//   pads never shift under it.
// - SDL's copy of an instrument the HID driver reads is left out, and the other
//   pads close up over its slot, as if it had never connected.
// - the keyboard and other synthetic devices feed player 1, as in the SDK, unless
//   a virtual instrument has player 1: then it has the slot to itself, so a type
//   change empties the slot and RB3 reads the new type when it comes back (RB3
//   only reads a pad's type when its slot connects).
static_assert(kPlayers == kMaxGuestUsers);

class PlayerAssignment final : public rex::input::DeviceAssignment {
public:
    void OnDevicesChanged(const std::vector<DeviceInfo>& devices) override {
        std::vector<SlotDevice> slots;
        slots.reserve(devices.size());
        for (const auto& device : devices) {
            SlotDevice slot;
            slot.ordinal = device.ordinal;
            if (int player = VirtualInstrumentPlayer(device)) {
                slot.kind = SlotDevice::Kind::kVirtual;
                slot.virtual_player = player;
            } else if (IsSdlCopyOfHidInstrument(device)) {
                slot.kind = SlotDevice::Kind::kSkipped;
            } else if (device.synthetic) {
                slot.kind = SlotDevice::Kind::kSynthetic;
            }
            slots.push_back(slot);
        }

        const auto players = AssignPlayers(slots, VirtualInstrumentPlayers());
        for (uint32_t user = 0; user < kMaxGuestUsers; user++) {
            users_[user].clear();
            for (size_t i : players[user]) users_[user].push_back(devices[i].id);
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
