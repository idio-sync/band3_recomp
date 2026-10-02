#include "native_socket.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#endif

namespace band3::net {

std::vector<uint32_t> ResolveIPv4(const std::string& host) {
#ifdef _WIN32
    // Winsock counts startups, so this one is harmless if the SDK's came first
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) return {};
#endif
    addrinfo hints{};
    hints.ai_family = AF_INET;
    addrinfo* found = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &found) != 0) return {};
    std::vector<uint32_t> addresses;
    for (const addrinfo* a = found; a; a = a->ai_next) {
        if (a->ai_family != AF_INET) continue;
        const uint32_t address = reinterpret_cast<const sockaddr_in*>(a->ai_addr)->sin_addr.s_addr;
        bool seen = false;
        for (uint32_t other : addresses) seen |= other == address;
        if (!seen) addresses.push_back(address);
    }
    freeaddrinfo(found);
    return addresses;
}

bool BoundAddress(uint64_t native_socket, uint16_t& port, uint32_t& address) {
    sockaddr_in local{};
#ifdef _WIN32
    int length = sizeof(local);
    const auto handle = static_cast<SOCKET>(native_socket);
#else
    socklen_t length = sizeof(local);
    const auto handle = static_cast<int>(native_socket);
#endif
    if (getsockname(handle, reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        local.sin_family != AF_INET) {
        return false;
    }
    port = local.sin_port;
    address = local.sin_addr.s_addr;
    return true;
}

int ReceiveFrom(uint64_t native_socket, uint8_t* buffer, size_t size, uint32_t& address,
                uint16_t& port, int wait_ms) {
#ifdef _WIN32
    const auto handle = static_cast<SOCKET>(native_socket);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(handle, &readable);
    timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
    if (ready < 0) return kSocketError;
    if (ready == 0) return kNothingWaiting;
    sockaddr_in from{};
    int from_length = sizeof(from);
    int received = recvfrom(handle, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0,
                            reinterpret_cast<sockaddr*>(&from), &from_length);
    if (received < 0) {
        const int error = WSAGetLastError();
        // an ICMP port unreachable from an earlier send, which UDP ignores
        if (error == WSAECONNRESET || error == WSAEWOULDBLOCK) return kNothingWaiting;
        // a datagram bigger than the buffer arrives cut short
        if (error != WSAEMSGSIZE) return kSocketError;
        received = static_cast<int>(size);
    }
    address = from.sin_addr.s_addr;
    port = from.sin_port;
    return received;
#else
    (void)native_socket, (void)buffer, (void)size, (void)address, (void)port, (void)wait_ms;
    return kSocketError;
#endif
}

int SendTo(uint64_t native_socket, const uint8_t* data, size_t size, uint32_t address,
           uint16_t port) {
#ifdef _WIN32
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = address;
    const int sent = sendto(static_cast<SOCKET>(native_socket), reinterpret_cast<const char*>(data),
                            static_cast<int>(size), 0, reinterpret_cast<const sockaddr*>(&to),
                            sizeof(to));
    return sent < 0 ? kSocketError : sent;
#else
    (void)native_socket, (void)data, (void)size, (void)address, (void)port;
    return kSocketError;
#endif
}

}
