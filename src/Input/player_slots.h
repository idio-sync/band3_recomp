#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Which player each input device feeds, kept apart from the SDK so it can be
// unit tested. src/Input/input_system.cpp's PlayerAssignment applies it.

namespace band3::input {

inline constexpr int kPlayers = 4;

struct SlotDevice {
    enum class Kind {
        kPad,        // a physical controller
        kSynthetic,  // the SDK's keyboard/mouse device or its stand-in
        kVirtual,    // a virtual instrument
        kSkipped,    // SDL's copy of an instrument the HID driver reads, a Stage Kit
    };
    Kind kind = Kind::kPad;
    // 1-4, for virtual instruments
    int virtual_player = 0;
    // for pads: the player (1-4) it had at the last assignment, 0 for none
    int seat = 0;
};

// For each player, the indices into `devices` of the devices that feed it.
// `devices` come in the SDK's order, by ordinal: a pad that connects takes the
// lowest free one. `reserved`: the players kept for virtual instruments,
// plugged in or replugging.
//
// A pad keeps its seat while it's connected and its player isn't reserved; the
// other pads take the lowest free players in order, so a pad never moves when
// another device comes or goes.
std::array<std::vector<size_t>, kPlayers> AssignPlayers(
    const std::vector<SlotDevice>& devices, const std::array<bool, kPlayers>& reserved);

// The player (0-3) the launcher reads one device on, alone (ReadInputCaps,
// ReadInputState): the last one it doesn't feed (`feeds`). The SDK remembers
// which device each player last had input from and prefers it among that
// player's devices, so a read on a player the device doesn't feed can't change
// which of player 1's devices the game first reads.
int ProbePlayer(const std::array<bool, kPlayers>& feeds);

// Controllers left out of the band (left_out_controllers): a pad there feeds no
// player, as if it weren't connected, though it still moves the launcher. A
// Steam Deck's own controls, docked away from the band, are the reason. Each
// is kept as "name|guid", separated by ';': the guid alone doesn't tell
// identical controllers apart, and under Steam Input every pad can share one,
// while SDL names a Steam pad for the controller behind it ("Steam Deck").

// the list's entry for a device
std::string LeftOutKey(std::string_view name, std::string_view guid);
// whether `list` has `key` (letter case aside, as guids' hex may differ)
bool IsLeftOut(std::string_view list, std::string_view key);
// `list` with `key` added, or taken out when it's there
std::string ToggleLeftOut(std::string_view list, std::string_view key);
// the list's keys, in order
std::vector<std::string> LeftOutKeys(std::string_view list);
// the name part of a key
std::string_view LeftOutName(std::string_view key);

// A Steam Deck's own controls (leave_out_deck_controls), by SDL's name for
// them or their USB ids (`vendor`, `product`; 0 when unknown). Under Steam
// Input SDL names a pad for the controller behind it.
bool IsSteamDeckControls(std::string_view name, uint16_t vendor, uint16_t product);

}
