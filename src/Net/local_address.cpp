#include "local_address.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace band3::net {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif

}

std::string LocalAddress() {
#ifdef _WIN32
    // game scripts ask for this with neither the events nor the web server on
    static const bool started = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    if (!started) return {};
#endif
    // connecting a UDP socket only picks the route, so nothing is sent
    socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) return {};
    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_port = htons(53);
    inet_pton(AF_INET, "8.8.8.8", &remote.sin_addr);
    std::string out;
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    if (connect(s, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote)) == 0 &&
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &length) == 0) {
        char addr[INET_ADDRSTRLEN] = {};
        inet_ntop(AF_INET, &local.sin_addr, addr, sizeof(addr));
        out = addr;
    }
    CloseSocket(s);
    return out;
}

}
