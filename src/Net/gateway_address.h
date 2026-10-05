#pragma once
#include <cstdint>

namespace band3::net {

// The IPv4 address of this machine's default gateway (its router), network
// order, or 0 when it has none. Asks the OS's routing table; sends nothing.
uint32_t DefaultGateway();

}
