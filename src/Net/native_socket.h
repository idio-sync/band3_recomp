#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace band3::net {

// The IPv4 addresses `host` resolves to on this machine, in network byte
// order; empty if it doesn't resolve.
std::vector<uint32_t> ResolveIPv4(const std::string& host);

// The local port and IPv4 address a host socket is bound to, both in network
// byte order; false if it isn't bound or isn't IPv4. Separate from the hooks,
// whose generated header has guest functions named like Winsock's.
bool BoundAddress(uint64_t native_socket, uint16_t& port, uint32_t& address);

// Takes one datagram waiting on a host UDP socket, waiting up to wait_ms for
// it: its length (cut to `size`), with the sender's address and port in
// network order; kNothingWaiting if none came, kSocketError if the socket failed.
inline constexpr int kNothingWaiting = -1;
inline constexpr int kSocketError = -2;
int ReceiveFrom(uint64_t native_socket, uint8_t* buffer, size_t size, uint32_t& address,
                uint16_t& port, int wait_ms);

// Sends a datagram to an IPv4 address and port, both in network order: the
// bytes sent, or kSocketError.
int SendTo(uint64_t native_socket, const uint8_t* data, size_t size, uint32_t address,
           uint16_t port);

// Why the last of these failed on this thread, as a Winsock error code, which
// the guest's are too (WSAENETDOWN off Windows).
uint32_t LastSocketError();

}
