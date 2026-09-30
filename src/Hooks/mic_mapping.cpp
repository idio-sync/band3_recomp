#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <cstdint>
#include <string>
#include <vector>
#include "generated/band3_init.h"
#include "src/Audio/mic_mapping_status.h"

// Watches which mic RB3 gives each player. MicClientMapper::RefreshPlayerMapping
// runs whenever the game's mics change (HandleMicsChanged); after it, each
// player's actual mic is decided. This only reads the result, for the Instrument
// Lab and the log: if a USB mic connects and is fed (Hooks/usb_mic.cpp) but no
// player holds its mic ID, the game side is where it stops. Layout is
// rb3-xenon's src/system/synth/MicClientMapper.h, matching the generated code.

extern "C" void __imp__MicClientMapper__RefreshPlayerMapping(PPCContext& ctx, uint8_t* base);

namespace {

using namespace band3::audio;

// std::vector<MicMappingData> mMappingData, 16-byte entries
constexpr uint32_t kMapper_MappingData = 0x4;
constexpr uint32_t kMappingData_Size = 0x10;
constexpr uint32_t kMappingData_MicID = 0x8;
constexpr uint32_t kMappingData_Locked = 0xC;
// std::vector<PlayerMappingData> mPlayers, 8-byte entries
constexpr uint32_t kMapper_Players = 0x10;
constexpr uint32_t kPlayerData_Size = 0x8;
constexpr uint32_t kPlayerData_Actual = 0x0;
constexpr uint32_t kPlayerData_Preferred = 0x4;

// more than the game ever has; a vector longer than this is misread
constexpr uint32_t kMaxEntries = 16;

// an STLport vector's [begin, end) as an entry count, 0 if it looks wrong
uint32_t Count(uint8_t* base, uint32_t vector, uint32_t entry_size) {
    const uint32_t begin = REX_LOAD_U32(vector);
    const uint32_t end = REX_LOAD_U32(vector + 4);
    if (!begin || end < begin || (end - begin) % entry_size) return 0;
    const uint32_t count = (end - begin) / entry_size;
    return count <= kMaxEntries ? count : 0;
}

std::string Describe(const std::vector<MicMappingPlayer>& players,
                     const std::vector<MicMappingMic>& mics) {
    std::string text = "mics [";
    for (size_t i = 0; i < mics.size(); i++) {
        if (i) text += ", ";
        text += std::to_string(mics[i].id);
        if (mics[i].locked) text += " (held)";
    }
    text += "], players [";
    for (size_t i = 0; i < players.size(); i++) {
        if (i) text += ", ";
        text += std::to_string(i + 1) + ": " +
                (players[i].actual < 0 ? std::string("none") : std::to_string(players[i].actual));
        if (players[i].preferred >= 0) text += " (wants " + std::to_string(players[i].preferred) + ")";
    }
    return text + "]";
}

}

// MicClientMapper::RefreshPlayerMapping(this)
extern "C" REX_FUNC(MicClientMapper__RefreshPlayerMapping)
{
    const uint32_t mapper = ctx.r3.u32;
    __imp__MicClientMapper__RefreshPlayerMapping(ctx, base);

    std::vector<MicMappingMic> mics;
    const uint32_t mic_vector = mapper + kMapper_MappingData;
    const uint32_t mic_begin = REX_LOAD_U32(mic_vector);
    for (uint32_t i = 0, n = Count(base, mic_vector, kMappingData_Size); i < n; i++) {
        const uint32_t entry = mic_begin + i * kMappingData_Size;
        mics.push_back({static_cast<int32_t>(REX_LOAD_U32(entry + kMappingData_MicID)),
                        REX_LOAD_U8(entry + kMappingData_Locked) != 0});
    }

    std::vector<MicMappingPlayer> players;
    const uint32_t player_vector = mapper + kMapper_Players;
    const uint32_t player_begin = REX_LOAD_U32(player_vector);
    for (uint32_t i = 0, n = Count(base, player_vector, kPlayerData_Size); i < n; i++) {
        const uint32_t entry = player_begin + i * kPlayerData_Size;
        players.push_back({static_cast<int32_t>(REX_LOAD_U32(entry + kPlayerData_Actual)),
                           static_cast<int32_t>(REX_LOAD_U32(entry + kPlayerData_Preferred))});
    }

    const std::string text = Describe(players, mics);
    if (RecordMicMapping(std::move(players), std::move(mics))) {
        REXLOG_INFO("Mic mapping: {}", text);
    }
}
