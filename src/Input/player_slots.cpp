#include "player_slots.h"

namespace band3::input {

std::array<std::vector<size_t>, kPlayers> AssignPlayers(
    const std::vector<SlotDevice>& devices, const std::array<bool, kPlayers>& reserved) {
    std::array<std::vector<size_t>, kPlayers> players;
    std::array<bool, kPlayers> taken = reserved;

    // the pads that keep their seat; the rest wait for the free players
    std::vector<size_t> unseated;
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
            const int seat = device.seat - 1;
            if (seat >= 0 && seat < kPlayers && !taken[seat]) {
                players[seat].push_back(i);
                taken[seat] = true;
            } else {
                unseated.push_back(i);
            }
            break;
        }
        }
    }

    for (size_t i : unseated) {
        for (int p = 0; p < kPlayers; p++) {
            if (taken[p]) continue;
            players[p].push_back(i);
            taken[p] = true;
            break;
        }
    }
    return players;
}

int ProbePlayer(const std::array<bool, kPlayers>& feeds) {
    for (int player = kPlayers - 1; player >= 0; player--) {
        if (!feeds[player]) return player;
    }
    // a device feeds one player at most; this is only for completeness
    return kPlayers - 1;
}

}
