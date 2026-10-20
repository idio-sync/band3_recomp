#pragma once
#include <string>

namespace band3::net {

// This machine's IPv4 address on the local network (the one its default route
// uses), or empty when it has none. Sends nothing.
std::string DefaultRouteAddress();

// DefaultRouteAddress, or with no default route the first other network's
// address (a hotspot the Steam Deck runs itself); empty when there's none.
std::string LocalAddress();

}
