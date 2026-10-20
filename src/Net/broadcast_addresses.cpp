#include "broadcast_addresses.h"
#include <algorithm>
#include "local_address.h"

#ifndef _WIN32
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#endif

namespace band3::net {

std::vector<uint32_t> OtherBroadcasts(std::span<const Interface> interfaces, uint32_t skip) {
    std::vector<uint32_t> out;
    for (const Interface& i : interfaces) {
        if (!i.up || i.loopback || !i.broadcast || i.address == 0 || i.address == skip) continue;
        const uint32_t broadcast = i.address | ~i.netmask;
        if (std::find(out.begin(), out.end(), broadcast) == out.end()) out.push_back(broadcast);
    }
    return out;
}

std::vector<Interface> LocalInterfaces() {
    std::vector<Interface> out;
#ifndef _WIN32
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) != 0) return out;
    for (const ifaddrs* a = list; a; a = a->ifa_next) {
        if (!a->ifa_addr || a->ifa_addr->sa_family != AF_INET || !a->ifa_netmask) continue;
        Interface i;
        i.address = reinterpret_cast<const sockaddr_in*>(a->ifa_addr)->sin_addr.s_addr;
        i.netmask = reinterpret_cast<const sockaddr_in*>(a->ifa_netmask)->sin_addr.s_addr;
        i.up = a->ifa_flags & IFF_UP;
        i.loopback = a->ifa_flags & IFF_LOOPBACK;
        i.broadcast = a->ifa_flags & IFF_BROADCAST;
        out.push_back(i);
    }
    freeifaddrs(list);
#endif
    return out;
}

std::vector<uint32_t> OtherNetworkBroadcasts() {
#ifdef _WIN32
    return {};
#else
    const std::vector<Interface> interfaces = LocalInterfaces();
    if (interfaces.empty()) return {};
    uint32_t routed = 0;
    if (const std::string local = DefaultRouteAddress(); !local.empty()) {
        inet_pton(AF_INET, local.c_str(), &routed);
    }
    return OtherBroadcasts(interfaces, routed);
#endif
}

}
