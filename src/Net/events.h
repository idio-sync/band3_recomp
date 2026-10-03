#pragma once
#include <cstddef>
#include <cstdint>

// RB3Enhanced-compatible network events: game state sent as UDP packets in
// RB3E's format, so existing receivers (Stage Kit bridges, dashboards) work
// unchanged. See include/net_events.h in the RB3Enhanced repo for the protocol.

namespace band3::events {

// values match RB3E_Events_EventTypes
enum EventType : uint8_t {
    kAlive = 0,          // string: build tag
    kState = 1,          // 1 byte: 00 = menus, 01 = in game
    kSongName = 2,       // string
    kSongArtist = 3,     // string
    kSongShortname = 4,  // string
    kStagekit = 6,       // 2 bytes: left channel, right channel
    kBandInfo = 7,       // BandInfo
    kVenueName = 8,      // string
    kScreenName = 9,     // string
    kDxData = 10,        // ModData
};

// what band3 calls itself in the alive event, where RB3E gives its build tag
// (rb3e_build_tag and the log give src/build_tag.h's)
inline constexpr char kBuildTag[] = "band3_recomp";

// RB3E_EventBandInfo
struct BandInfo {
    uint8_t member_exists[4];
    uint8_t difficulty[4];
    uint8_t track_type[4];
};

// RB3E_EventModData: arbitrary strings sent by mods such as RB3 Deluxe
struct ModData {
    char identify_value[10];
    char string[240];
};

static_assert(sizeof(BandInfo) == 12);
static_assert(sizeof(ModData) == 250);

// true when [events] enabled is set; hooks check this before reading game state
bool Enabled();

// sends one event when enabled; opens the socket on first use
void Send(EventType type, const void* data, size_t size);

// sends a string event without its terminator, as RB3E does
void SendString(EventType type, const char* str);

}
