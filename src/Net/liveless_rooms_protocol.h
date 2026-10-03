#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

// The Liveless Rooms protocol: RB3Enhanced's matchmaking server, where a player
// logs in for a code and others join them by it. Packets as RB3Enhanced's
// net_liveless_online.h packs them on a PowerPC: integers big-endian, IPv4
// addresses as their four bytes in order. Encoding and decoding only; the
// connection is the caller's.
namespace band3::rooms {

inline constexpr uint16_t kPort = 19532;
inline constexpr uint16_t kMagic = 0x4C4C;  // "LL"
inline constexpr uint8_t kProtocolVersion = 0;
inline constexpr size_t kHeaderSize = 6, kMaxBody = 0x300, kProofSize = 0x228;

// Each direction numbers its packets from 0, so a type means nothing without one.
enum class ClientType : uint8_t { Hello = 0, Login = 1, Pong = 2, JoinRequest = 3 };
enum class ServerType : uint8_t {
    Hello = 0,
    LoggedIn = 1,
    Ping = 2,
    JoinResponse = 3,
    JoinDenied = 4,
    NatPunchRequest = 5
};

using Bytes = std::vector<uint8_t>;

// IPv4 addresses below are uint32s in network order, as band3's sockets hold
// them. Strings longer than their field are cut to it.

// The first packet, on connecting. RB3Enhanced sends its build tag as the
// version, which the server may show; band3 sends its own. `emulator` tells the
// server not to expect a console's signature in the login proof.
struct ClientHello {
    bool emulator = true;
    std::string language = "eng";  // three letters
    std::string version;           // up to 47 characters
};
// The reply: whether to log in, and the key to prove it with.
struct ServerHello {
    bool allowed;
    bool needs_proof;
    std::array<uint8_t, 16> proof_key;
};
// `proof` is LoginProof's, when the server asks for one; without, zeros are sent.
struct ClientLogin {
    uint64_t xuid;
    std::string gamertag;  // up to 15 characters
    uint32_t local_ipv4;   // the address on the player's own network
    std::optional<std::array<uint8_t, 20>> proof;
};
// Logged in: the player's address as the server saw it, and their code.
struct ServerLoggedIn {
    uint32_t public_ipv4;
    std::string code;  // 8 characters
};
// Join the player with this code. The server ignores case; RB3Enhanced's codes
// are upper case.
struct JoinRequest {
    std::string code;
};
// To the player joining: the host's name and addresses. join_type 0 is a code
// the player entered, 1 a join from outside the game.
struct JoinResponse {
    uint8_t join_type;
    std::string user;  // up to 15 characters
    uint64_t xuid;
    uint32_t public_ipv4;
    uint32_t private_ipv4;
};
// No such code (reason 0, the only one).
struct JoinDenied {
    uint8_t reason;
};
// To the host: someone at this address is joining, so open the way to them.
struct NatPunchRequest {
    uint32_t public_ipv4;
};
// The server's keepalive and the client's answer, the same bytes both ways.
struct Ping {};
struct Pong {};
// A packet type this codec doesn't know, which RB3Enhanced logs and ignores.
struct Unknown {
    uint8_t type;
};

// Whole frames, header included.
Bytes Encode(const ClientHello& hello);
Bytes Encode(const ClientLogin& login);
Bytes Encode(const Pong& pong);
Bytes Encode(const JoinRequest& request);
Bytes Encode(const ServerHello& hello);
Bytes Encode(const ServerLoggedIn& logged_in);
Bytes Encode(const Ping& ping);
Bytes Encode(const JoinResponse& response);
Bytes Encode(const JoinDenied& denied);
Bytes Encode(const NatPunchRequest& request);

struct Frame {
    uint8_t type;
    Bytes body;
};

// Splits a TCP stream into frames by the header's size. RB3Enhanced itself
// reads one packet per recv() and ignores the size, so a server must send its
// packets one by one; band3 doesn't count on that.
class FrameReader {
public:
    void Feed(const uint8_t* data, size_t size);
    // The next whole frame, or nothing until more comes (or ever, once broken).
    std::optional<Frame> Next();
    // Whether the stream had a header without the magic, or a body over
    // kMaxBody: it isn't Rooms, or lost its place, so drop the connection.
    bool Broken() const { return broken_; }

private:
    Bytes buffer_;
    bool broken_ = false;
};

using ServerMessage =
    std::variant<ServerHello, ServerLoggedIn, Ping, JoinResponse, JoinDenied, NatPunchRequest, Unknown>;
using ClientMessage = std::variant<ClientHello, ClientLogin, Pong, JoinRequest, Unknown>;

// A frame's packet; nothing if its body is shorter than the packet's (a longer
// one is read as far as the packet goes). Strings end at their first NUL.
std::optional<ServerMessage> DecodeServer(const Frame& frame);
// band3 is only ever a client, so this is for tests and the mock's side of things. A
// login's proof decodes as nothing if it's all zeros, else its first 20 bytes; it doesn't
// check that the rest is zero, so it can't tell an emulator's hash from a console's
// signature.
std::optional<ClientMessage> DecodeClient(const Frame& frame);

// The proof a login sends when the server asks: HMAC-SHA1 of the server's
// address, as the player typed it, and the XUID's eight bytes, keyed with
// ServerHello's key. RB3Enhanced on a console signs this; on Xenia it sends it
// as is, which is what band3 does too.
std::array<uint8_t, 20> LoginProof(const std::array<uint8_t, 16>& key, std::string_view server_address,
                                   uint64_t xuid);

// The XUID band3 logs in to Rooms with, from its username, since there's no
// Xbox Live to give it one: an offline XUID's 0x0009 top, and the low 48 bits of
// the name's 64-bit FNV-1a (never zero). Only the login sends it; the game
// keeps its own.
uint64_t RoomsXuid(std::string_view username);

}
