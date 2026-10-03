#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace band3::online {

// The GoCentral server RB3Enhanced's Xbox 360 builds connect to.
inline constexpr std::string_view kDefaultGoCentral = "gocentral-xbox.rbenhanced.rocks";

// The UDP port RB3 plays online on, as RB3Enhanced's Liveless has it.
inline constexpr uint16_t kGamePort = 9103;

// Whether RB3 asking Quazal for `host` means Rock Central: one of Harmonix's
// *.hmxservices.com servers, or the stand-in address Quazal logs into when it
// bypasses the Xbox Live secure gateway. RB3Enhanced sends both to GoCentral.
bool IsRockCentralHost(std::string_view host);

// Whether `username` can be a player's GoCentral account. GoCentral knows an
// Xbox player by gamertag alone, so a blank one, or the "User" every profile
// here starts as (band3_config.ini's too), would be everyone's account.
bool IsOwnAccountName(std::string_view username);

struct Endpoint {
    std::string host;
    uint16_t port;
};

// "host" or "host:port" (a name or an IPv4 address), with `default_port`
// when it has none; nothing if it's blank or the port isn't 1-65535.
std::optional<Endpoint> ParseEndpoint(std::string_view text, uint16_t default_port);

// liveless_connect's game to join. Without a port, one on this PC (127.0.0.1,
// localhost: hosting) is this game, on `own_port`, and another player's is on
// RB3Enhanced's 9103.
std::optional<Endpoint> ParseJoinAddress(std::string_view text, uint16_t own_port);

}
