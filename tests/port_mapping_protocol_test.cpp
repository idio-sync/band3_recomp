// Checks the PCP and NAT-PMP codec (src/Net/port_mapping_protocol.cpp) against
// tests/golden/port_mapping_packets.txt, the packets the mock router
// (tools/port_mapping_mock.py) is tested against too, and the address checks
// beside it.

#include <doctest/doctest.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>
#include "src/Net/port_mapping_protocol.h"

using namespace band3::port_mapping;

namespace {

const std::map<std::string, std::string>& Golden() {
    static const std::map<std::string, std::string> golden = [] {
        std::map<std::string, std::string> lines;
        std::ifstream file(std::filesystem::path(BAND3_GOLDEN_DIR) / "port_mapping_packets.txt");
        std::string line;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            std::istringstream fields(line);
            std::string name, hex;
            fields >> name >> hex;
            lines[name] = hex;
        }
        return lines;
    }();
    return golden;
}

std::string GoldenHex(const std::string& name) {
    const auto it = Golden().find(name);
    REQUIRE_MESSAGE(it != Golden().end(), "no golden vector " << name);
    return it->second;
}

std::vector<uint8_t> GoldenBytes(const std::string& name) {
    const std::string hex = GoldenHex(name);
    std::vector<uint8_t> bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

template <typename T>
std::string Hex(const T& bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string hex;
    for (uint8_t b : bytes) {
        hex += kDigits[b >> 4];
        hex += kDigits[b & 15];
    }
    return hex;
}

Reply ParseGolden(const std::string& name) {
    const auto bytes = GoldenBytes(name);
    return ParseReply(bytes.data(), bytes.size());
}

// a.b.c.d in network order, as sockets keep it
uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t bytes[4] = {a, b, c, d};
    uint32_t ipv4;
    std::memcpy(&ipv4, bytes, 4);
    return ipv4;
}

constexpr Nonce kNonce = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
const uint32_t kClient = Ip(192, 168, 1, 20);
const uint32_t kPublic = Ip(203, 0, 113, 5);

}  // namespace

TEST_CASE("PCP MAP requests are the golden bytes: map for an hour, and delete") {
    const auto map = EncodePcpMap(kNonce, kClient, 9103, 3600);
    CHECK(map.size() == 60);
    CHECK(Hex(map) == GoldenHex("pcp_map_request"));
    CHECK(Hex(EncodePcpMap(kNonce, kClient, 9103, 0)) == GoldenHex("pcp_delete_request"));
}

TEST_CASE("NAT-PMP requests are the golden bytes") {
    CHECK(Hex(EncodeNatPmpExternalAddress()) == GoldenHex("natpmp_external_address_request"));
    CHECK(Hex(EncodeNatPmpMap(9103, 9103, 3600)) == GoldenHex("natpmp_map_request"));
    // RFC 6886: a delete asks for external port 0 with lifetime 0
    CHECK(Hex(EncodeNatPmpMap(9103, 0, 0)) == GoldenHex("natpmp_delete_request"));
}

TEST_CASE("a PCP MAP reply gives the mapping, the nonce and the public address") {
    const Reply reply = ParseGolden("pcp_map_reply");
    CHECK(reply.kind == Reply::Kind::kPcpMap);
    CHECK(reply.result == 0);
    CHECK(reply.lifetime_s == 3600);
    CHECK(reply.nonce == kNonce);
    CHECK(reply.internal_port == 9103);
    CHECK(reply.external_port == 9103);
    CHECK(reply.external_ipv4 == kPublic);

    const Reply deleted = ParseGolden("pcp_delete_reply");
    CHECK(deleted.kind == Reply::Kind::kPcpMap);
    CHECK(deleted.lifetime_s == 0);

    CHECK(ParseGolden("pcp_map_reply_other_port").external_port == 9104);
}

TEST_CASE("a PCP failure comes without the MAP part") {
    const Reply reply = ParseGolden("pcp_map_reply_not_authorized");
    CHECK(reply.kind == Reply::Kind::kPcpMap);
    CHECK(reply.result == 2);
    CHECK(reply.external_port == 0);
    CHECK(PcpResultText(reply.result) == "PCP error 2 (not authorized)");
    CHECK(PcpResultText(99) == "PCP error 99");
}

TEST_CASE("result 1 is an unsupported version, whichever protocol's header it comes in") {
    // a NAT-PMP router's answer to PCP, and a PCP-only router's to NAT-PMP
    for (const char* name : {"unsupported_version_natpmp", "unsupported_version_pcp"}) {
        CAPTURE(name);
        const Reply reply = ParseGolden(name);
        CHECK(reply.kind == Reply::Kind::kUnsupportedVersion);
        CHECK(reply.result == kUnsupportedVersion);
    }
}

TEST_CASE("NAT-PMP replies give the public address, and the mapping") {
    const Reply address = ParseGolden("natpmp_external_address_reply");
    CHECK(address.kind == Reply::Kind::kNatPmpExternalAddress);
    CHECK(address.result == 0);
    CHECK(address.external_ipv4 == kPublic);

    const Reply map = ParseGolden("natpmp_map_reply");
    CHECK(map.kind == Reply::Kind::kNatPmpMap);
    CHECK(map.result == 0);
    CHECK(map.internal_port == 9103);
    CHECK(map.external_port == 9103);
    CHECK(map.lifetime_s == 3600);

    const Reply deleted = ParseGolden("natpmp_delete_reply");
    CHECK(deleted.kind == Reply::Kind::kNatPmpMap);
    CHECK(deleted.external_port == 0);
    CHECK(deleted.lifetime_s == 0);

    const Reply refused = ParseGolden("natpmp_map_reply_refused");
    CHECK(refused.kind == Reply::Kind::kNatPmpMap);
    CHECK(refused.result == 2);
    CHECK(NatPmpResultText(refused.result) == "NAT-PMP error 2 (not authorized or refused)");
}

TEST_CASE("what isn't a whole reply band3 asks for is invalid") {
    // requests (no reply bit), a short success, an opcode band3 never sends
    for (const char* name : {"pcp_map_request", "natpmp_map_request", "natpmp_external_address_request"}) {
        CAPTURE(name);
        CHECK(ParseGolden(name).kind == Reply::Kind::kInvalid);
    }
    auto map = GoldenBytes("pcp_map_reply");
    CHECK(ParseReply(map.data(), 59).kind == Reply::Kind::kInvalid);
    auto natpmp = GoldenBytes("natpmp_map_reply");
    CHECK(ParseReply(natpmp.data(), 15).kind == Reply::Kind::kInvalid);
    natpmp[1] = 0x82;  // a TCP mapping's reply
    CHECK(ParseReply(natpmp.data(), natpmp.size()).kind == Reply::Kind::kInvalid);
    map[0] = 3;  // a version neither protocol has
    CHECK(ParseReply(map.data(), map.size()).kind == Reply::Kind::kInvalid);
    CHECK(ParseReply(map.data(), 3).kind == Reply::Kind::kInvalid);
}

TEST_CASE("private addresses aren't where players over the internet reach this PC") {
    for (uint32_t ip : {Ip(10, 1, 2, 3), Ip(172, 16, 0, 1), Ip(172, 31, 255, 255), Ip(192, 168, 1, 1),
                        Ip(100, 64, 0, 1), Ip(100, 127, 1, 1), Ip(127, 0, 0, 1), Ip(169, 254, 3, 4),
                        Ip(0, 0, 0, 0)}) {
        CAPTURE(ip);
        CHECK(IsPrivateAddress(ip));
    }
    for (uint32_t ip : {kPublic, Ip(8, 8, 8, 8), Ip(172, 32, 0, 1), Ip(172, 15, 0, 1),
                        Ip(100, 128, 0, 1), Ip(192, 169, 1, 1), Ip(198, 51, 100, 9)}) {
        CAPTURE(ip);
        CHECK_FALSE(IsPrivateAddress(ip));
    }
}

TEST_CASE("the default gateway is read from /proc/net/route") {
    const std::string route =
        "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
        "eth0\t0000A8C0\t00000000\t0001\t0\t0\t100\t00FFFFFF\t0\t0\t0\n"
        "eth0\t00000000\t0101A8C0\t0003\t0\t0\t100\t00000000\t0\t0\t0\n";
    CHECK(ParseDefaultGateway(route) == Ip(192, 168, 1, 1));
    // no default route, or one that isn't up through a gateway
    CHECK(ParseDefaultGateway(
              "Iface\tDestination\tGateway\tFlags\n"
              "eth0\t0000A8C0\t00000000\t0001\n") == 0);
    CHECK(ParseDefaultGateway(
              "Iface\tDestination\tGateway\tFlags\n"
              "eth0\t00000000\t0101A8C0\t0002\n") == 0);
    CHECK(ParseDefaultGateway("") == 0);
}
