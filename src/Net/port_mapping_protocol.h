#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

// The router's side of a port mapping, as bytes: PCP (RFC 6887) MAP requests
// and NAT-PMP (RFC 6886) requests, and the replies to both, which share a
// header. RB3Enhanced's net_natpmp.c asks the same way. Plain bytes, no
// sockets: src/Net/port_mapping.cpp sends them, tests/port_mapping_protocol_test.cpp
// and tools/port_mapping_mock.py check them against tests/golden. Ports and
// lifetimes are in host order; IPv4 addresses in network order, as sockets
// keep them.
namespace band3::port_mapping {

// where a router takes PCP and NAT-PMP requests
inline constexpr uint16_t kGatewayPort = 5351;
inline constexpr uint8_t kNatPmpVersion = 0;
inline constexpr uint8_t kPcpVersion = 2;
inline constexpr uint8_t kPcpOpMap = 1;
inline constexpr uint8_t kNatPmpOpExternalAddress = 0;
inline constexpr uint8_t kNatPmpOpMapUdp = 1;
// a reply's opcode is its request's with this set
inline constexpr uint8_t kReplyBit = 0x80;
inline constexpr uint8_t kProtocolUdp = 17;
// both protocols' "unsupported version": a router that only speaks the other
inline constexpr uint8_t kUnsupportedVersion = 1;

inline constexpr size_t kPcpMapSize = 60;
inline constexpr size_t kNatPmpExternalAddressSize = 2;
inline constexpr size_t kNatPmpMapSize = 12;
inline constexpr size_t kNatPmpExternalAddressReplySize = 12;
inline constexpr size_t kNatPmpMapReplySize = 16;

// what PCP knows a mapping by, for renewing and deleting it
using Nonce = std::array<uint8_t, 12>;

// PCP MAP for a UDP port, the same inside and out, with no preference for
// the external address. `client_ipv4` is this PC's address as it reaches the
// router (the router checks it against where the request came from);
// `lifetime_s` 0 deletes the mapping `nonce` made.
std::array<uint8_t, kPcpMapSize> EncodePcpMap(const Nonce& nonce, uint32_t client_ipv4,
                                              uint16_t port, uint32_t lifetime_s);
// NAT-PMP's two requests: the router's external address, and a UDP mapping
// (lifetime 0 deletes it)
std::array<uint8_t, kNatPmpExternalAddressSize> EncodeNatPmpExternalAddress();
std::array<uint8_t, kNatPmpMapSize> EncodeNatPmpMap(uint16_t internal_port,
                                                    uint16_t external_port,
                                                    uint32_t lifetime_s);

// A reply from the router, either protocol's.
struct Reply {
    enum class Kind {
        kInvalid,              // too short, or not a reply band3 asks for
        kPcpMap,               // PCP MAP
        kNatPmpExternalAddress,
        kNatPmpMap,
        kUnsupportedVersion,   // result 1 to either protocol, whatever its version byte says
    };
    Kind kind = Kind::kInvalid;
    uint8_t result = 0;        // 0 is success
    uint32_t lifetime_s = 0;
    uint32_t external_ipv4 = 0;
    uint16_t internal_port = 0;
    uint16_t external_port = 0;
    Nonce nonce{};             // PCP's
};

Reply ParseReply(const uint8_t* data, size_t size);

// A result code as text, for the log and the harness's error field: "PCP
// error 2 (not authorized)".
std::string PcpResultText(uint8_t result);
std::string NatPmpResultText(uint8_t result);

// Whether an address is one only a private network reaches (10/8, 172.16/12,
// 192.168/16, 100.64/10 carrier-grade NAT, 127/8, 169.254/16, 0/8): a router that reports
// one as its external address is behind another, so it isn't where players
// over the internet reach this PC. Network order.
bool IsPrivateAddress(uint32_t ipv4);

// The default route's gateway in /proc/net/route's text, network order, or 0
// for none. The file prints each address as the kernel holds it, a
// little-endian word on the machines band3 runs on.
uint32_t ParseDefaultGateway(std::string_view proc_net_route);

}  // namespace band3::port_mapping
