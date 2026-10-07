#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// The few MQTT 3.1.1 packets band3 needs to tell Home Assistant what the game is
// doing: CONNECT (with a last will), PUBLISH at QoS 0, PINGREQ and DISCONNECT
// out, CONNACK and PINGRESP in. Bytes only, no sockets, so it's tested byte for
// byte; mqtt_client.h puts it on a connection.
namespace band3::mqtt {

using Bytes = std::vector<uint8_t>;

struct Message {
    std::string topic;
    std::string payload;
    bool retain = false;
};

// what the broker publishes for us when the connection drops without a DISCONNECT
struct Will {
    std::string topic;
    std::string payload;
    bool retain = true;
};

struct ConnectOptions {
    std::string client_id;
    std::string username;  // empty: no user name flag
    std::string password;  // only sent with a user name
    std::optional<Will> will;
    uint16_t keepalive_s = 60;
};

enum PacketType : uint8_t {
    kConnect = 1,
    kConnack = 2,
    kPublish = 3,
    kPingReq = 12,
    kPingResp = 13,
    kDisconnect = 14,
};

// MQTT 3.1.1 (protocol level 4), clean session
Bytes EncodeConnect(const ConnectOptions& options);
// QoS 0: no packet id, nothing comes back
Bytes EncodePublish(const Message& message);
Bytes EncodePingReq();
Bytes EncodeDisconnect();
// the fixed header's length: 7 bits a byte, low first, the top bit saying
// another follows; 1-4 bytes, up to 268435455
void AppendRemainingLength(Bytes& out, uint32_t length);

// one packet: the type and flags of its first byte, and what follows its length
struct Packet {
    uint8_t type = 0;
    uint8_t flags = 0;
    Bytes body;
};

// Bytes from the broker, in whatever pieces they came, into whole packets.
class PacketReader {
public:
    void Add(const uint8_t* data, size_t size);
    // the next whole packet, or nothing until more bytes come (or once Bad)
    std::optional<Packet> Next();
    // a malformed length (more than 4 length bytes): the connection should close
    bool Bad() const { return bad_; }

private:
    Bytes buffer_;
    bool bad_ = false;
};

// the return code of a CONNACK, -1 if `packet` isn't a valid one
int ConnackCode(const Packet& packet);
// why the broker refused a connection, by its CONNACK's code
std::string ConnackText(int code);

}  // namespace band3::mqtt
