#pragma once
#include <cstdint>
#include <vector>

// Which game mic each player sings through, as RB3's MicClientMapper decides it
// (Hooks/mic_mapping.cpp), for the Instrument Lab. The game re-decides whenever
// its mics change: a player keeps the mic they prefer while it is connected and
// free, and anyone left over gets the first free one.

namespace band3::audio {

struct MicMappingPlayer {
    // the mic ID the player sings through, -1 for none
    int32_t actual = -1;
    // the mic ID the player asked for, -1 for any
    int32_t preferred = -1;

    bool operator==(const MicMappingPlayer&) const = default;
};

struct MicMappingMic {
    int32_t id = -1;
    // held by a player
    bool locked = false;

    bool operator==(const MicMappingMic&) const = default;
};

struct MicMapping {
    // false until the game first assigns its mics
    bool seen = false;
    uint64_t refreshes = 0;
    // by player ID
    std::vector<MicMappingPlayer> players;
    // the connected mics the game knows about
    std::vector<MicMappingMic> mics;
};

// From the hook, after each MicClientMapper::RefreshPlayerMapping. Returns
// whether anything differs from the last one, so the hook logs only changes.
bool RecordMicMapping(std::vector<MicMappingPlayer> players, std::vector<MicMappingMic> mics);

MicMapping GetMicMapping();

}
