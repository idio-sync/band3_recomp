#include "port_mapping_protocol.h"
#include <cstring>
#include <iterator>
#include <sstream>

namespace band3::port_mapping {

namespace {

void Put16(uint8_t* out, uint16_t value) {
    out[0] = static_cast<uint8_t>(value >> 8);
    out[1] = static_cast<uint8_t>(value);
}

void Put32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value >> 24);
    out[1] = static_cast<uint8_t>(value >> 16);
    out[2] = static_cast<uint8_t>(value >> 8);
    out[3] = static_cast<uint8_t>(value);
}

uint16_t Get16(const uint8_t* in) { return static_cast<uint16_t>(in[0] << 8 | in[1]); }

uint32_t Get32(const uint8_t* in) {
    return uint32_t{in[0]} << 24 | uint32_t{in[1]} << 16 | uint32_t{in[2]} << 8 | in[3];
}

// PCP carries IPv4 addresses as IPv4-mapped IPv6 (::ffff:a.b.c.d); the
// all-zeros IPv4 address, "no preference", is ::ffff:0.0.0.0 (RFC 6887
// section 5), as RB3Enhanced sends it
void PutMappedIpv4(uint8_t* out, uint32_t ipv4) {
    std::memset(out, 0, 10);
    out[10] = 0xFF;
    out[11] = 0xFF;
    std::memcpy(out + 12, &ipv4, 4);
}

uint32_t GetIpv4(const uint8_t* in) {
    uint32_t ipv4;
    std::memcpy(&ipv4, in, 4);
    return ipv4;
}

// NAT-PMP's result is 16 bits; none it defines needs more than 8
uint8_t NatPmpResult(const uint8_t* data) {
    const uint16_t result = Get16(data + 2);
    return result > 0xFF ? 0xFF : static_cast<uint8_t>(result);
}

std::string ResultText(const char* protocol, uint8_t result, const char* name) {
    std::string text = std::string(protocol) + " error " + std::to_string(result);
    if (name) text += std::string(" (") + name + ")";
    return text;
}

}  // namespace

std::array<uint8_t, kPcpMapSize> EncodePcpMap(const Nonce& nonce, uint32_t client_ipv4,
                                              uint16_t port, uint32_t lifetime_s) {
    // header (24): version, opcode, 2 reserved, lifetime, client address;
    // MAP (36): nonce, protocol, 3 reserved, internal and suggested external
    // port, suggested external address
    std::array<uint8_t, kPcpMapSize> out{};
    out[0] = kPcpVersion;
    out[1] = kPcpOpMap;
    Put32(&out[4], lifetime_s);
    PutMappedIpv4(&out[8], client_ipv4);
    std::memcpy(&out[24], nonce.data(), nonce.size());
    out[36] = kProtocolUdp;
    Put16(&out[40], port);
    Put16(&out[42], port);
    PutMappedIpv4(&out[44], 0);
    return out;
}

std::array<uint8_t, kNatPmpExternalAddressSize> EncodeNatPmpExternalAddress() {
    return {kNatPmpVersion, kNatPmpOpExternalAddress};
}

std::array<uint8_t, kNatPmpMapSize> EncodeNatPmpMap(uint16_t internal_port,
                                                    uint16_t external_port,
                                                    uint32_t lifetime_s) {
    // version, opcode, 2 reserved, internal port, suggested external port, lifetime
    std::array<uint8_t, kNatPmpMapSize> out{};
    out[0] = kNatPmpVersion;
    out[1] = kNatPmpOpMapUdp;
    Put16(&out[4], internal_port);
    Put16(&out[6], external_port);
    Put32(&out[8], lifetime_s);
    return out;
}

Reply ParseReply(const uint8_t* data, size_t size) {
    Reply reply;
    // version, opcode with the reply bit, and the result in byte 3 for both
    // (PCP: reserved, result; NAT-PMP: a 16-bit result)
    if (size < 4 || !(data[1] & kReplyBit)) return reply;
    const uint8_t version = data[0], opcode = data[1] & ~kReplyBit;
    // a router that doesn't speak the version asked answers in its own, so
    // this is told by the result alone
    if (data[2] == 0 && data[3] == kUnsupportedVersion) {
        reply.kind = Reply::Kind::kUnsupportedVersion;
        reply.result = kUnsupportedVersion;
        return reply;
    }
    if (version == kPcpVersion && opcode == kPcpOpMap) {
        // a failure may come without the MAP part
        reply.result = data[3];
        if (size < 24 || (reply.result == 0 && size < kPcpMapSize)) return reply;
        reply.kind = Reply::Kind::kPcpMap;
        reply.lifetime_s = Get32(data + 4);
        if (size >= kPcpMapSize) {
            std::memcpy(reply.nonce.data(), data + 24, reply.nonce.size());
            reply.internal_port = Get16(data + 40);
            reply.external_port = Get16(data + 42);
            reply.external_ipv4 = GetIpv4(data + 56);
        }
        return reply;
    }
    if (version != kNatPmpVersion) return reply;
    reply.result = NatPmpResult(data);
    if (opcode == kNatPmpOpExternalAddress) {
        if (size < 8 || (reply.result == 0 && size < kNatPmpExternalAddressReplySize)) return reply;
        reply.kind = Reply::Kind::kNatPmpExternalAddress;
        if (size >= kNatPmpExternalAddressReplySize) reply.external_ipv4 = GetIpv4(data + 8);
    } else if (opcode == kNatPmpOpMapUdp) {
        if (size < 8 || (reply.result == 0 && size < kNatPmpMapReplySize)) return reply;
        reply.kind = Reply::Kind::kNatPmpMap;
        if (size >= kNatPmpMapReplySize) {
            reply.internal_port = Get16(data + 8);
            reply.external_port = Get16(data + 10);
            reply.lifetime_s = Get32(data + 12);
        }
    }
    return reply;
}

std::string PcpResultText(uint8_t result) {
    // RFC 6887 section 7.4
    static constexpr const char* kNames[] = {
        "success", "unsupported version", "not authorized", "malformed request",
        "unsupported opcode", "unsupported option", "malformed option", "network failure",
        "no resources", "unsupported protocol", "user quota exceeded",
        "cannot provide external", "address mismatch", "excessive remote peers"};
    return ResultText("PCP", result, result < std::size(kNames) ? kNames[result] : nullptr);
}

std::string NatPmpResultText(uint8_t result) {
    // RFC 6886 section 3.5
    static constexpr const char* kNames[] = {
        "success", "unsupported version", "not authorized or refused", "network failure",
        "out of resources", "unsupported opcode"};
    return ResultText("NAT-PMP", result, result < std::size(kNames) ? kNames[result] : nullptr);
}

bool IsPrivateAddress(uint32_t ipv4) {
    uint8_t b[4];
    std::memcpy(b, &ipv4, 4);
    return b[0] == 0 || b[0] == 10 || b[0] == 127 ||
           (b[0] == 172 && (b[1] & 0xF0) == 16) ||
           (b[0] == 192 && b[1] == 168) ||
           (b[0] == 100 && (b[1] & 0xC0) == 64) ||
           (b[0] == 169 && b[1] == 254);  // link-local: no DHCP answered
}

uint32_t ParseDefaultGateway(std::string_view proc_net_route) {
    // Iface Destination Gateway Flags RefCnt Use Metric Mask..., a header line
    // first; the default route goes to 00000000 through a gateway (RTF_UP and
    // RTF_GATEWAY set)
    constexpr unsigned long kUp = 0x1, kGateway = 0x2;
    std::istringstream lines{std::string(proc_net_route)};
    std::string line;
    while (std::getline(lines, line)) {
        std::istringstream fields(line);
        std::string iface, destination, gateway, flags;
        if (!(fields >> iface >> destination >> gateway >> flags)) continue;
        if (destination != "00000000" || gateway.size() != 8) continue;
        unsigned long word = 0, bits = 0;
        try {
            word = std::stoul(gateway, nullptr, 16);
            bits = std::stoul(flags, nullptr, 16);
        } catch (...) {
            continue;
        }
        if ((bits & (kUp | kGateway)) != (kUp | kGateway) || word == 0) continue;
        // the little-endian word's bytes, lowest first, are the address's in
        // network order
        const uint8_t bytes[4] = {static_cast<uint8_t>(word), static_cast<uint8_t>(word >> 8),
                                  static_cast<uint8_t>(word >> 16),
                                  static_cast<uint8_t>(word >> 24)};
        uint32_t ipv4;
        std::memcpy(&ipv4, bytes, 4);
        return ipv4;
    }
    return 0;
}

}  // namespace band3::port_mapping
