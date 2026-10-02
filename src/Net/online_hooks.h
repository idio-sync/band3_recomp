#pragma once
#include <cstdint>

namespace band3::online {

// Finds the SDK's XAM user exports that band3's sign-in overrides hand on to;
// false if any is missing. Call before the game runs.
bool ResolveSdkExports();

// Reads the gocentral and liveless settings and turns on what they need, as
// RB3Enhanced does on a console with Xbox Live blocked. Call before the game
// runs.
void Start();

// Set by Start, for the hooks:
// players count as signed in to Live, and Quazal skips Live's security
bool LiveSpoofed();
// Rock Central goes to GoCentral
bool GoCentral();
// online sessions go straight to another player
bool Liveless();

// Liveless: the port of the game to join (from liveless_connect), the port
// this game plays on (liveless_port), both in host order, and this PC's
// address as others reach it, IPv4 in network order.
uint16_t LivelessJoinPort();
uint16_t LivelessPort();
uint32_t LivelessExternalAddress();

// The address RB3Enhanced's session search hands the game for the game to
// join (192.0.2.1, reserved for documentation), which Quazal then looks up by
// name; band3 sends it to the real one. Network order.
inline constexpr uint32_t kJoinStandIn = 0x010200C0;
inline constexpr const char* kJoinStandInName = "192.0.2.1";

}
