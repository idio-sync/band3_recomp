#include "mqtt_protocol.h"

#include <algorithm>

namespace band3::mqtt {

namespace {

// the largest a remaining length can say, in its 4 bytes
constexpr uint32_t kMaxRemainingLength = 268435455;

void AppendU16(Bytes& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value & 0xFF));
}

// a UTF-8 string as MQTT has them, after its length; longer than a u16 can
// say is cut (nothing band3 sends comes near it)
void AppendString(Bytes& out, const std::string& text) {
    const size_t size = std::min<size_t>(text.size(), 0xFFFF);
    AppendU16(out, static_cast<uint16_t>(size));
    out.insert(out.end(), text.begin(), text.begin() + static_cast<std::ptrdiff_t>(size));
}

// the fixed header, then `body`
Bytes Frame(uint8_t first, const Bytes& body) {
    Bytes out;
    out.reserve(body.size() + 5);
    out.push_back(first);
    AppendRemainingLength(out, static_cast<uint32_t>(std::min<size_t>(body.size(), kMaxRemainingLength)));
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

}  // namespace

void AppendRemainingLength(Bytes& out, uint32_t length) {
    length = std::min(length, kMaxRemainingLength);
    do {
        uint8_t byte = static_cast<uint8_t>(length & 0x7F);
        length >>= 7;
        if (length > 0) byte |= 0x80;
        out.push_back(byte);
    } while (length > 0);
}

Bytes EncodeConnect(const ConnectOptions& options) {
    const bool user = !options.username.empty();
    // a password without a user name isn't allowed in 3.1.1
    const bool password = user && !options.password.empty();
    uint8_t flags = 0x02;  // clean session: nothing kept between connections
    if (options.will) {
        flags |= 0x04;  // will, at QoS 0
        if (options.will->retain) flags |= 0x20;
    }
    if (password) flags |= 0x40;
    if (user) flags |= 0x80;

    Bytes body;
    AppendString(body, "MQTT");
    body.push_back(4);  // protocol level: 3.1.1
    body.push_back(flags);
    AppendU16(body, options.keepalive_s);
    AppendString(body, options.client_id);
    if (options.will) {
        AppendString(body, options.will->topic);
        AppendString(body, options.will->payload);
    }
    if (user) AppendString(body, options.username);
    if (password) AppendString(body, options.password);
    return Frame(static_cast<uint8_t>(kConnect << 4), body);
}

Bytes EncodePublish(const Message& message) {
    Bytes body;
    AppendString(body, message.topic);
    // QoS 0 has no packet id, and the payload runs to the packet's end
    body.insert(body.end(), message.payload.begin(), message.payload.end());
    return Frame(static_cast<uint8_t>((kPublish << 4) | (message.retain ? 1 : 0)), body);
}

Bytes EncodePingReq() { return {static_cast<uint8_t>(kPingReq << 4), 0}; }

Bytes EncodeDisconnect() { return {static_cast<uint8_t>(kDisconnect << 4), 0}; }

void PacketReader::Add(const uint8_t* data, size_t size) {
    if (bad_ || size == 0) return;
    buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<Packet> PacketReader::Next() {
    if (bad_ || buffer_.size() < 2) return std::nullopt;
    uint32_t length = 0;
    size_t at = 1;
    for (int shift = 0;; shift += 7) {
        if (at >= buffer_.size()) return std::nullopt;
        const uint8_t byte = buffer_[at++];
        length |= static_cast<uint32_t>(byte & 0x7F) << shift;
        if (!(byte & 0x80)) break;
        // a fourth byte that says another follows: no length is that long
        if (at - 1 == 4) {
            bad_ = true;
            buffer_.clear();
            return std::nullopt;
        }
    }
    if (buffer_.size() - at < length) return std::nullopt;
    Packet packet;
    packet.type = static_cast<uint8_t>(buffer_[0] >> 4);
    packet.flags = static_cast<uint8_t>(buffer_[0] & 0x0F);
    const auto begin = buffer_.begin() + static_cast<std::ptrdiff_t>(at);
    const auto end = begin + static_cast<std::ptrdiff_t>(length);
    packet.body.assign(begin, end);
    buffer_.erase(buffer_.begin(), end);
    return packet;
}

int ConnackCode(const Packet& packet) {
    if (packet.type != kConnack || packet.body.size() != 2) return -1;
    return packet.body[1];
}

std::string ConnackText(int code) {
    switch (code) {
    case 1: return "unacceptable protocol version";
    case 2: return "identifier rejected";
    case 3: return "server unavailable";
    case 4: return "bad user name or password";
    case 5: return "not authorized";
    default: return "refused (code " + std::to_string(code) + ")";
    }
}

}  // namespace band3::mqtt
