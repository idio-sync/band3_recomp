// Checks the Liveless Rooms codec (src/Net/liveless_rooms_protocol.cpp) and the
// SHA-1 under its login proof (src/Net/sha1.cpp) against tests/golden, the
// vectors the mock server (tools/liveless_rooms_mock.py) tests against too.

#include <doctest/doctest.h>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include "src/Net/liveless_rooms_protocol.h"
#include "src/Net/sha1.h"

using namespace band3::rooms;

namespace {

// The golden file's "name hex" lines, by name.
const std::map<std::string, std::string>& Golden() {
    static const std::map<std::string, std::string> golden = [] {
        std::map<std::string, std::string> lines;
        std::ifstream file(std::filesystem::path(BAND3_GOLDEN_DIR) / "liveless_rooms_packets.txt");
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

std::string Hex(const uint8_t* data, size_t size) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string hex;
    for (size_t i = 0; i < size; i++) {
        hex += kDigits[data[i] >> 4];
        hex += kDigits[data[i] & 15];
    }
    return hex;
}

template <typename T>
std::string Hex(const T& bytes) {
    return Hex(bytes.data(), bytes.size());
}

std::string Hex(uint64_t xuid) {
    char text[17];
    std::snprintf(text, sizeof(text), "%016llX", static_cast<unsigned long long>(xuid));
    return text;
}

Bytes FromHex(const std::string& hex) {
    Bytes bytes;
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoi(hex.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

Bytes GoldenBytes(const std::string& name) { return FromHex(GoldenHex(name)); }

// A golden frame, split off by the reader the way band3 receives one.
Frame GoldenFrame(const std::string& name) {
    const Bytes bytes = GoldenBytes(name);
    FrameReader reader;
    reader.Feed(bytes.data(), bytes.size());
    auto frame = reader.Next();
    REQUIRE(frame);
    CHECK_FALSE(reader.Next());
    CHECK_FALSE(reader.Broken());
    return *frame;
}

Frame MakeFrame(uint8_t type, size_t body_size) { return Frame{type, Bytes(body_size, 0)}; }

// An IPv4 address as band3 holds one: a uint32 in network order.
uint32_t Ip(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    const uint8_t bytes[4] = {a, b, c, d};
    uint32_t address;
    std::memcpy(&address, bytes, 4);
    return address;
}

// The golden file's inputs
std::array<uint8_t, 16> GoldenKey() {
    std::array<uint8_t, 16> key;
    for (size_t i = 0; i < key.size(); i++) key[i] = static_cast<uint8_t>(i);
    return key;
}
constexpr uint64_t kXuid = 0x0009000000000001;
const uint32_t kLocal = Ip(192, 168, 1, 2);
const uint32_t kPublic = Ip(127, 0, 0, 1);

std::array<uint8_t, 20> GoldenProof() {
    std::array<uint8_t, 20> proof{};
    const Bytes bytes = GoldenBytes("proof");
    REQUIRE(bytes.size() == proof.size());
    std::memcpy(proof.data(), bytes.data(), proof.size());
    return proof;
}

std::span<const uint8_t> Text(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

template <typename T, typename Message>
T As(const std::optional<Message>& message) {
    REQUIRE(message);
    REQUIRE(std::holds_alternative<T>(*message));
    return std::get<T>(*message);
}

// The bytes of a frame's body, a field at `offset` of `size`.
std::string BodyField(const Bytes& frame, size_t offset, size_t size) {
    REQUIRE(frame.size() >= kHeaderSize + offset + size);
    return std::string(reinterpret_cast<const char*>(frame.data()) + kHeaderSize + offset, size);
}

}

TEST_CASE("SHA-1 and HMAC-SHA1 match their published vectors") {
    using band3::net::HmacSha1;
    using band3::net::Sha1;
    CHECK(Hex(Sha1(Text("abc"))) == GoldenHex("sha1_abc"));
    CHECK(Hex(Sha1(Text(""))) == "DA39A3EE5E6B4B0D3255BFEF95601890AFD80709");
    // 56 bytes: the length no longer fits the first block's padding
    CHECK(Hex(Sha1(Text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"))) ==
          "84983E441C3BD26EBAAE4AA1F95129E5E54670F1");
    // RFC 2202's first two: a 20-byte key of 0x0B, then the key "Jefe"
    const Bytes key1(20, 0x0B);
    CHECK(Hex(HmacSha1(key1, Text("Hi There"))) == GoldenHex("hmac_rfc2202_1"));
    CHECK(Hex(HmacSha1(Text("Jefe"), Text("what do ya want for nothing?"))) ==
          GoldenHex("hmac_rfc2202_2"));
    // RFC 2202's sixth: a key longer than a block is hashed first
    const Bytes key6(80, 0xAA);
    CHECK(Hex(HmacSha1(key6, Text("Test Using Larger Than Block-Size Key - Hash Key First"))) ==
          "AA4AE5E15272D00E95705637CE8A3B55ED402112");
}

TEST_CASE("the login proof is an HMAC of the server's address and the XUID") {
    CHECK(Hex(LoginProof(GoldenKey(), "127.0.0.1", kXuid)) == GoldenHex("proof"));
}

TEST_CASE("a Rooms XUID comes from the username") {
    CHECK(Hex(RoomsXuid("host")) == GoldenHex("xuid_host"));
    CHECK(Hex(RoomsXuid("joiner")) == GoldenHex("xuid_joiner"));
    CHECK(Hex(RoomsXuid("User")) == GoldenHex("xuid_User"));
    CHECK(RoomsXuid("host") != RoomsXuid("Host"));
}

TEST_CASE("client packets encode as RB3Enhanced sends them") {
    CHECK(Hex(Encode(ClientHello{true, "eng", "band3-test"})) == GoldenHex("client_hello"));
    CHECK(Hex(Encode(ClientLogin{kXuid, "host", kLocal, GoldenProof()})) == GoldenHex("client_login"));
    CHECK(Hex(Encode(JoinRequest{"ABCD2345"})) == GoldenHex("join_request"));
    CHECK(Hex(Encode(Pong{})) == GoldenHex("pong"));
}

TEST_CASE("server packets encode as the Rooms server sends them") {
    CHECK(Hex(Encode(ServerHello{true, true, GoldenKey()})) == GoldenHex("server_hello"));
    CHECK(Hex(Encode(ServerLoggedIn{kPublic, "ABCD2345"})) == GoldenHex("server_logged_in"));
    CHECK(Hex(Encode(JoinResponse{0, "host", kXuid, kPublic, kLocal})) == GoldenHex("join_response"));
    CHECK(Hex(Encode(JoinDenied{0})) == GoldenHex("join_denied"));
    CHECK(Hex(Encode(NatPunchRequest{kPublic})) == GoldenHex("nat_punch"));
    CHECK(Hex(Encode(Ping{})) == GoldenHex("ping"));
    // the two are the same packet, each direction's type 2
    CHECK(Encode(Ping{}) == Encode(Pong{}));
}

TEST_CASE("server packets decode") {
    const auto& hello = As<ServerHello>(DecodeServer(GoldenFrame("server_hello")));
    CHECK(hello.allowed);
    CHECK(hello.needs_proof);
    CHECK(hello.proof_key == GoldenKey());

    const auto& logged_in = As<ServerLoggedIn>(DecodeServer(GoldenFrame("server_logged_in")));
    CHECK(logged_in.public_ipv4 == kPublic);
    CHECK(logged_in.code == "ABCD2345");

    const auto& response = As<JoinResponse>(DecodeServer(GoldenFrame("join_response")));
    CHECK(response.join_type == 0);
    CHECK(response.user == "host");
    CHECK(response.xuid == kXuid);
    CHECK(response.public_ipv4 == kPublic);
    CHECK(response.private_ipv4 == kLocal);

    CHECK(As<JoinDenied>(DecodeServer(GoldenFrame("join_denied"))).reason == 0);
    CHECK(As<NatPunchRequest>(DecodeServer(GoldenFrame("nat_punch"))).public_ipv4 == kPublic);
    As<Ping>(DecodeServer(GoldenFrame("ping")));
}

TEST_CASE("client packets decode") {
    const auto& hello = As<ClientHello>(DecodeClient(GoldenFrame("client_hello")));
    CHECK(hello.emulator);
    CHECK(hello.language == "eng");
    CHECK(hello.version == "band3-test");

    const auto& login = As<ClientLogin>(DecodeClient(GoldenFrame("client_login")));
    CHECK(login.xuid == kXuid);
    CHECK(login.gamertag == "host");
    CHECK(login.local_ipv4 == kLocal);
    REQUIRE(login.proof);
    CHECK(*login.proof == GoldenProof());

    CHECK(As<JoinRequest>(DecodeClient(GoldenFrame("join_request"))).code == "ABCD2345");
    As<Pong>(DecodeClient(GoldenFrame("pong")));
}

TEST_CASE("a login without a proof sends zeros, and decodes as none") {
    const Bytes frame = Encode(ClientLogin{kXuid, "host", kLocal, std::nullopt});
    REQUIRE(frame.size() == kHeaderSize + 580);
    CHECK(BodyField(frame, 28, kProofSize) == std::string(kProofSize, '\0'));
    const Frame parsed{static_cast<uint8_t>(ClientType::Login), Bytes(frame.begin() + kHeaderSize, frame.end())};
    CHECK_FALSE(As<ClientLogin>(DecodeClient(parsed)).proof);
}

TEST_CASE("strings are cut and padded to their fields") {
    // a gamertag keeps 15 characters and its NUL
    const Bytes login = Encode(ClientLogin{kXuid, "ABCDEFGHIJKLMNOP", kLocal, std::nullopt});
    CHECK(BodyField(login, 8, 16) == std::string("ABCDEFGHIJKLMNO") + '\0');
    // so does a JoinResponse's
    const Bytes response = Encode(JoinResponse{0, "ABCDEFGHIJKLMNOP", kXuid, kPublic, kLocal});
    CHECK(BodyField(response, 1, 16) == std::string("ABCDEFGHIJKLMNO") + '\0');
    // a version keeps 47 and its NUL
    const Bytes hello = Encode(ClientHello{true, "eng", std::string(60, 'v')});
    REQUIRE(hello.size() == kHeaderSize + 53);
    CHECK(BodyField(hello, 5, 0x30) == std::string(47, 'v') + '\0');
    // a language is exactly three letters, with no NUL
    CHECK(BodyField(Encode(ClientHello{false, "english", "x"}), 1, 4) == std::string("\0eng", 4));
    CHECK(BodyField(Encode(ClientHello{true, "en", "x"}), 2, 3) == std::string("en\0", 3));
    // a code is eight characters, NUL-padded when shorter, never NUL-terminated
    CHECK(BodyField(Encode(JoinRequest{"AB12"}), 0, 8) == std::string("AB12\0\0\0\0", 8));
    CHECK(BodyField(Encode(JoinRequest{"ABCD23456"}), 0, 8) == "ABCD2345");
    CHECK(Encode(JoinRequest{"ABCD23456"}).size() == kHeaderSize + 8);
    CHECK(BodyField(Encode(ServerLoggedIn{kPublic, "AB"}), 4, 8) == std::string("AB\0\0\0\0\0\0", 8));
}

TEST_CASE("decoded strings stop at the first NUL or the field's end") {
    Frame frame = GoldenFrame("server_logged_in");
    frame.body[4 + 3] = 0;
    CHECK(As<ServerLoggedIn>(DecodeServer(frame)).code == "ABC");

    frame = GoldenFrame("join_response");
    std::memset(frame.body.data() + 1, 'Z', 16);
    CHECK(As<JoinResponse>(DecodeServer(frame)).user == std::string(16, 'Z'));
    frame.body[1 + 2] = 0;
    frame.body[1 + 5] = 'Q';
    CHECK(As<JoinResponse>(DecodeServer(frame)).user == "ZZ");
}

TEST_CASE("a body shorter than its packet doesn't decode; a longer one does") {
    CHECK_FALSE(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::Hello), 17)));
    CHECK_FALSE(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::LoggedIn), 11)));
    CHECK_FALSE(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::JoinResponse), 32)));
    CHECK_FALSE(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::JoinDenied), 0)));
    CHECK_FALSE(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::NatPunchRequest), 3)));
    CHECK_FALSE(DecodeClient(MakeFrame(static_cast<uint8_t>(ClientType::Hello), 52)));
    CHECK_FALSE(DecodeClient(MakeFrame(static_cast<uint8_t>(ClientType::Login), 579)));
    CHECK_FALSE(DecodeClient(MakeFrame(static_cast<uint8_t>(ClientType::JoinRequest), 7)));

    Frame frame = GoldenFrame("join_response");
    frame.body.resize(frame.body.size() + 10, 0xEE);
    CHECK(As<JoinResponse>(DecodeServer(frame)).user == "host");
    As<Ping>(DecodeServer(MakeFrame(static_cast<uint8_t>(ServerType::Ping), 4)));
}

TEST_CASE("a type the codec doesn't know decodes as Unknown") {
    CHECK(As<Unknown>(DecodeServer(MakeFrame(6, 0))).type == 6);
    CHECK(As<Unknown>(DecodeServer(MakeFrame(0xFF, 3))).type == 0xFF);
    CHECK(As<Unknown>(DecodeClient(MakeFrame(4, 0))).type == 4);
}

TEST_CASE("the reader frames a stream fed a byte at a time") {
    Bytes stream = GoldenBytes("server_hello");
    const Bytes logged_in = GoldenBytes("server_logged_in");
    const Bytes ping = GoldenBytes("ping");
    stream.insert(stream.end(), logged_in.begin(), logged_in.end());
    stream.insert(stream.end(), ping.begin(), ping.end());

    FrameReader reader;
    std::vector<Frame> frames;
    for (uint8_t byte : stream) {
        reader.Feed(&byte, 1);
        while (auto frame = reader.Next()) frames.push_back(*frame);
    }
    CHECK_FALSE(reader.Broken());
    REQUIRE(frames.size() == 3);
    CHECK(frames[0].type == static_cast<uint8_t>(ServerType::Hello));
    CHECK(frames[0].body.size() == 18);
    CHECK(frames[1].type == static_cast<uint8_t>(ServerType::LoggedIn));
    CHECK(Hex(frames[1].body) == GoldenHex("server_logged_in").substr(kHeaderSize * 2));
    CHECK(frames[2].type == static_cast<uint8_t>(ServerType::Ping));
    CHECK(frames[2].body.empty());
}

TEST_CASE("the reader splits two frames fed together") {
    Bytes stream = GoldenBytes("join_denied");
    const Bytes nat = GoldenBytes("nat_punch");
    stream.insert(stream.end(), nat.begin(), nat.end());
    // and the start of a third, which waits for the rest
    stream.insert(stream.end(), {0x4C, 0x4C, 0x00});

    FrameReader reader;
    reader.Feed(stream.data(), stream.size());
    auto first = reader.Next();
    auto second = reader.Next();
    REQUIRE(first);
    REQUIRE(second);
    CHECK(first->type == static_cast<uint8_t>(ServerType::JoinDenied));
    CHECK(first->body == Bytes{0x00});
    CHECK(second->type == static_cast<uint8_t>(ServerType::NatPunchRequest));
    CHECK(second->body == Bytes{0x7F, 0x00, 0x00, 0x01});
    CHECK_FALSE(reader.Next());
    CHECK_FALSE(reader.Broken());

    const uint8_t rest[] = {0x02, 0x00, 0x00};
    reader.Feed(rest, sizeof(rest));
    auto third = reader.Next();
    REQUIRE(third);
    CHECK(third->type == static_cast<uint8_t>(ServerType::Ping));
}

TEST_CASE("the reader gives up on a stream that isn't Rooms") {
    FrameReader bad_magic;
    const uint8_t http[] = {'H', 'T', 'T', 'P', '/', '1', '.', '1'};
    bad_magic.Feed(http, sizeof(http));
    CHECK_FALSE(bad_magic.Next());
    CHECK(bad_magic.Broken());
    // and stays broken, whatever follows
    const Bytes ping = GoldenBytes("ping");
    bad_magic.Feed(ping.data(), ping.size());
    CHECK_FALSE(bad_magic.Next());
    CHECK(bad_magic.Broken());

    FrameReader too_big;
    const uint8_t header[] = {0x4C, 0x4C, 0x00, 0x01, 0x03, 0x01};
    too_big.Feed(header, sizeof(header));
    CHECK_FALSE(too_big.Next());
    CHECK(too_big.Broken());

    // the largest body there can be is fine
    FrameReader largest;
    Bytes frame = {0x4C, 0x4C, 0x00, 0x01, 0x03, 0x00};
    frame.resize(kHeaderSize + kMaxBody, 0);
    largest.Feed(frame.data(), frame.size());
    auto parsed = largest.Next();
    REQUIRE(parsed);
    CHECK(parsed->body.size() == kMaxBody);
    CHECK_FALSE(largest.Broken());
}
