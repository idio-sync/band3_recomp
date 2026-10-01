#pragma once
#include <string>

namespace band3::net {

// This machine's IPv4 address on the local network (the one its default route
// uses), or empty when it has none. Sends nothing.
std::string LocalAddress();

}
