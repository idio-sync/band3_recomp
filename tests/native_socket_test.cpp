// Checks band3's host-side UDP (src/Net/native_socket.cpp), which Quazal's
// sends, receives and getsockname go through: a datagram round trip on this
// machine's loopback.

#include <doctest/doctest.h>
#include <cstring>
#include "src/Net/native_socket.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace {

#ifdef _WIN32
using Handle = SOCKET;
void Close(Handle s) { closesocket(s); }
#else
using Handle = int;
void Close(Handle s) { close(s); }
#endif

// a UDP socket on 127.0.0.1, on a port the system picks
Handle LoopbackSocket() {
    const Handle s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    return s;
}

}  // namespace

TEST_CASE("a name resolves to its IPv4 address, in network order") {
    // also starts Winsock for the rest
    const auto found = band3::net::ResolveIPv4("127.0.0.1");
    REQUIRE(found.size() == 1);
    CHECK(found[0] == htonl(INADDR_LOOPBACK));
}

TEST_CASE("a datagram goes from one socket to another, with its sender") {
    REQUIRE_FALSE(band3::net::ResolveIPv4("127.0.0.1").empty());
    const Handle from = LoopbackSocket(), to = LoopbackSocket();
    uint16_t from_port = 0, to_port = 0;
    uint32_t from_address = 0, to_address = 0;
    REQUIRE(band3::net::BoundAddress(from, from_port, from_address));
    REQUIRE(band3::net::BoundAddress(to, to_port, to_address));
    CHECK(from_address == htonl(INADDR_LOOPBACK));
    CHECK(from_port != 0);

    uint8_t buffer[64] = {};
    uint32_t sender = 0;
    uint16_t sender_port = 0;
    CHECK(band3::net::ReceiveFrom(to, buffer, sizeof(buffer), sender, sender_port, 0) ==
          band3::net::kNothingWaiting);

    const uint8_t hello[] = {'b', 'a', 'n', 'd', '3'};
    CHECK(band3::net::SendTo(from, hello, sizeof(hello), to_address, to_port) == sizeof(hello));
    REQUIRE(band3::net::ReceiveFrom(to, buffer, sizeof(buffer), sender, sender_port, 1000) ==
            sizeof(hello));
    CHECK(std::memcmp(buffer, hello, sizeof(hello)) == 0);
    CHECK(sender == from_address);
    CHECK(sender_port == from_port);

    Close(from);
    Close(to);
}
