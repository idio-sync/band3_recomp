#include "player_slots.h"

namespace band3::input {

std::array<std::vector<size_t>, kPlayers> AssignPlayers(
    const std::vector<SlotDevice>& devices, const std::array<bool, kPlayers>& reserved) {
    std::array<std::vector<size_t>, kPlayers> players;

    std::vector<uint32_t> skipped;
    for (const auto& device : devices) {
        if (device.kind == SlotDevice::Kind::kSkipped) skipped.push_back(device.ordinal);
    }

    for (size_t i = 0; i < devices.size(); i++) {
        const SlotDevice& device = devices[i];
        switch (device.kind) {
        case SlotDevice::Kind::kVirtual:
            if (device.virtual_player >= 1 && device.virtual_player <= kPlayers) {
                players[device.virtual_player - 1].push_back(i);
            }
            break;
        case SlotDevice::Kind::kSkipped:
            break;
        case SlotDevice::Kind::kSynthetic:
            // a virtual instrument on player 1 has the slot to itself, so a type
            // change empties it and RB3 reads the new type when it comes back
            if (!reserved[0]) players[0].push_back(i);
            break;
        case SlotDevice::Kind::kPad: {
            // the pads close up over any skipped copy that connected before them,
            // then take the free players in order
            uint32_t nth = device.ordinal;
            for (uint32_t s : skipped) {
                if (s < device.ordinal) nth--;
            }
            for (int p = 0; p < kPlayers; p++) {
                if (reserved[p]) continue;
                if (nth == 0) {
                    players[p].push_back(i);
                    break;
                }
                nth--;
            }
            break;
        }
        }
    }
    return players;
}

}
