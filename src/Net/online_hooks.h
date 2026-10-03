#pragma once
#include <cstdint>
#include <string>

namespace band3::online {

// Finds the SDK's XAM user exports that band3's sign-in overrides hand on to;
// false if any is missing. Call before the game runs.
bool ResolveSdkExports();

// Reads the gocentral and liveless settings and turns on what they need, as
// RB3Enhanced does on a console with Xbox Live blocked. Call before the game
// runs.
void Start();

// Ends what Start began that runs on threads of its own (the Liveless Rooms
// client). Call at shutdown, before the test server stops and the kernel goes.
void Stop();

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

// Liveless: makes `host`:`port` the game to join from now on, as
// liveless_connect does at launch; false if guest memory ran out.
bool SetLivelessJoin(const std::string& host, uint16_t port);

// Liveless: has the game accept an invite to the game at `host`:`port`, as
// the console does when the player accepts one from the guide: it's told of
// the invite (XN_LIVE_INVITE_ACCEPTED, for player 1), its
// XInviteGetAcceptedInfo then reads one for that game, and the join goes
// there. `force_flag` also sets the BandUI's "joined through an invite" flag
// first. Returns an error, or empty.
std::string FakeInvite(const std::string& host, uint16_t port, bool force_flag);

// The address RB3Enhanced's session search hands the game for the game to
// join (192.0.2.1, reserved for documentation), which Quazal then looks up by
// name; band3 sends it to the real one. Network order.
inline constexpr uint32_t kJoinStandIn = 0x010200C0;
inline constexpr const char* kJoinStandInName = "192.0.2.1";

}
