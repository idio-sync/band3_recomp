#include "native_socket.h"
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#endif

namespace band3::net {

namespace {

#ifdef _WIN32
using Handle = SOCKET;
using Length = int;
#else
using Handle = int;
using Length = socklen_t;
#endif

Handle ToHandle(uint64_t native_socket) { return static_cast<Handle>(native_socket); }

}  // namespace

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
    Length length = sizeof(local);
    if (getsockname(ToHandle(native_socket), reinterpret_cast<sockaddr*>(&local), &length) != 0 ||
        local.sin_family != AF_INET) {
        return false;
    }
    port = local.sin_port;
    address = local.sin_addr.s_addr;
    return true;
}

int ReceiveFrom(uint64_t native_socket, uint8_t* buffer, size_t size, uint32_t& address,
                uint16_t& port, int wait_ms) {
    const Handle handle = ToHandle(native_socket);
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(handle, &readable);
    timeval timeout{wait_ms / 1000, (wait_ms % 1000) * 1000};
#ifdef _WIN32
    const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
    const int ready = select(handle + 1, &readable, nullptr, nullptr, &timeout);
#endif
    if (ready < 0) return kSocketError;
    if (ready == 0) return kNothingWaiting;
    sockaddr_in from{};
    Length from_length = sizeof(from);
    int received = static_cast<int>(recvfrom(handle, reinterpret_cast<char*>(buffer),
                                             static_cast<int>(size), 0,
                                             reinterpret_cast<sockaddr*>(&from), &from_length));
    if (received < 0) {
#ifdef _WIN32
        const int error = WSAGetLastError();
        // an ICMP port unreachable from an earlier send, which UDP ignores
        if (error == WSAECONNRESET || error == WSAEWOULDBLOCK) return kNothingWaiting;
        // a datagram bigger than the buffer arrives cut short
        if (error != WSAEMSGSIZE) return kSocketError;
        received = static_cast<int>(size);
#else
        if (errno == ECONNREFUSED || errno == EAGAIN || errno == EWOULDBLOCK) return kNothingWaiting;
        return kSocketError;
#endif
    }
    address = from.sin_addr.s_addr;
    port = from.sin_port;
    return received;
}

int SendTo(uint64_t native_socket, const uint8_t* data, size_t size, uint32_t address,
           uint16_t port) {
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = port;
    to.sin_addr.s_addr = address;
    const int sent = static_cast<int>(sendto(ToHandle(native_socket),
                                             reinterpret_cast<const char*>(data),
                                             static_cast<int>(size), 0,
                                             reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
    return sent < 0 ? kSocketError : sent;
}

uint32_t LastSocketError() {
#ifdef _WIN32
    return static_cast<uint32_t>(WSAGetLastError());
#else
    return 10050;  // WSAENETDOWN
#endif
}

}
