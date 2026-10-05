#include "gateway_address.h"
#include "port_mapping_protocol.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <iphlpapi.h>
#else
#include <fstream>
#include <sstream>
#endif

namespace band3::net {

uint32_t DefaultGateway() {
#ifdef _WIN32
    // the route a public address takes is the default one; its next hop is
    // the router (8.8.8.8 reads the same in either byte order)
    constexpr DWORD kPublic = 0x08080808;
    MIB_IPFORWARDROW row{};
    if (GetBestRoute(kPublic, 0, &row) != NO_ERROR) return 0;
    // 0 when the address is on this machine's own network: no router
    return row.dwForwardNextHop;
#else
    std::ifstream file("/proc/net/route");
    if (!file) return 0;
    std::stringstream text;
    text << file.rdbuf();
    return port_mapping::ParseDefaultGateway(text.str());
#endif
}

}
