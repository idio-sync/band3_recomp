#pragma once
#include <cstdint>
#include <span>
#include <vector>

// The broadcast addresses of the networks this PC's default route doesn't
// reach: a Wi-Fi hotspot the Steam Deck runs itself (src/Lights/hotspot.h),
// whose devices 255.255.255.255 misses, as it leaves by the default route (and
// can't leave at all with no default route).

namespace band3::net {

// one of this PC's IPv4 addresses; address and netmask in network order
struct Interface {
    uint32_t address = 0;
    uint32_t netmask = 0;
    bool up = false;
    bool loopback = false;
    // its network takes broadcasts (not a point-to-point link)
    bool broadcast = false;
};

// the broadcast address of each up, broadcast-taking, non-loopback interface
// but those whose address is `skip` (the default route's; 0 for none), each once
std::vector<uint32_t> OtherBroadcasts(std::span<const Interface> interfaces, uint32_t skip);

// this PC's interfaces; empty on Windows
std::vector<Interface> LocalInterfaces();

// OtherBroadcasts of this PC's interfaces and its default route's address
// (DefaultRouteAddress); empty on Windows
std::vector<uint32_t> OtherNetworkBroadcasts();

}
