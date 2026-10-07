// Checks the MQTT 3.1.1 packets band3 sends Home Assistant's broker
// (src/Net/mqtt_protocol.cpp) against hand-checked bytes, and the reading of
// what the broker sends back.

#include <doctest/doctest.h>
#include <cstdio>
#include <string>
#include "src/Net/mqtt_protocol.h"

using namespace band3::mqtt;

namespace {

// bytes as "10 0c 00 04", for messages that show where they differ
std::string Hex(const Bytes& bytes) {
    std::string text;
    char part[4];
    for (size_t i = 0; i < bytes.size(); i++) {
        std::snprintf(part, sizeof(part), i ? " %02x" : "%02x", bytes[i]);
        text += part;
    }
    return text;
}

// `text`'s characters as bytes
Bytes Ascii(const std::string& text) { return Bytes(text.begin(), text.end()); }

Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& part : parts) out.insert(out.end(), part.begin(), part.end());
    return out;
}

Bytes Length(uint32_t length) {
    Bytes out;
    AppendRemainingLength(out, length);
    return out;
}

}  // namespace

TEST_CASE("mqtt CONNECT with only a client id") {
    ConnectOptions options;
    options.client_id = "band3";
    options.keepalive_s = 60;
    const Bytes expected = Join({
        {0x10, 17},                                    // CONNECT, 10 + 7
        {0x00, 0x04}, Ascii("MQTT"), {0x04},           // protocol name, level 4
        {0x02},                                        // clean session
        {0x00, 0x3C},                                  // keepalive 60
        {0x00, 0x05}, Ascii("band3"),                  // client id
    });
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt CONNECT with a user name and password") {
    ConnectOptions options;
    options.client_id = "c";
    options.username = "ha";
    options.password = "pw!";
    options.keepalive_s = 30;
    const Bytes expected = Join({
        {0x10, 22},  // 10 + 3 + 4 + 5
        {0x00, 0x04}, Ascii("MQTT"), {0x04},
        {0xC2},  // user name, password, clean session
        {0x00, 0x1E},
        {0x00, 0x01}, Ascii("c"),
        {0x00, 0x02}, Ascii("ha"),
        {0x00, 0x03}, Ascii("pw!"),
    });
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt CONNECT with a retained will and a user name") {
    ConnectOptions options;
    options.client_id = "band3_pc";
    options.username = "u";
    options.password = "p";
    options.will = Will{"band3/pc/status", "offline", true};
    const Bytes expected = Join({
        {0x10, 52},  // 10 + 10 + 17 + 9 + 3 + 3
        {0x00, 0x04}, Ascii("MQTT"), {0x04},
        {0xE6},  // user name, password, will retain, will, clean session; will QoS 0
        {0x00, 0x3C},
        {0x00, 0x08}, Ascii("band3_pc"),
        {0x00, 0x0F}, Ascii("band3/pc/status"),
        {0x00, 0x07}, Ascii("offline"),
        {0x00, 0x01}, Ascii("u"),
        {0x00, 0x01}, Ascii("p"),
    });
    // the length byte is the rest of the packet
    REQUIRE(expected.size() == 2 + 52);
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt CONNECT with a will not retained") {
    ConnectOptions options;
    options.client_id = "x";
    options.will = Will{"t", "w", false};
    const Bytes expected = Join({
        {0x10, 19},
        {0x00, 0x04}, Ascii("MQTT"), {0x04},
        {0x06},  // will, clean session
        {0x00, 0x3C},
        {0x00, 0x01}, Ascii("x"),
        {0x00, 0x01}, Ascii("t"),
        {0x00, 0x01}, Ascii("w"),
    });
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt CONNECT with a user name and no password has no password flag") {
    ConnectOptions options;
    options.client_id = "c";
    options.username = "ha";
    const Bytes expected = Join({
        {0x10, 17},
        {0x00, 0x04}, Ascii("MQTT"), {0x04},
        {0x82},
        {0x00, 0x3C},
        {0x00, 0x01}, Ascii("c"),
        {0x00, 0x02}, Ascii("ha"),
    });
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt CONNECT sends no password without a user name") {
    ConnectOptions options;
    options.client_id = "c";
    options.password = "secret";
    const Bytes expected = Join({
        {0x10, 13},
        {0x00, 0x04}, Ascii("MQTT"), {0x04},
        {0x02},
        {0x00, 0x3C},
        {0x00, 0x01}, Ascii("c"),
    });
    CHECK(Hex(EncodeConnect(options)) == Hex(expected));
}

TEST_CASE("mqtt PUBLISH at QoS 0, retained and not") {
    const Bytes plain = Join({{0x30, 12}, {0x00, 0x05}, Ascii("a/b/c"), Ascii("hello")});
    CHECK(Hex(EncodePublish(Message{"a/b/c", "hello", false})) == Hex(plain));
    const Bytes retained = Join({{0x31, 12}, {0x00, 0x05}, Ascii("a/b/c"), Ascii("hello")});
    CHECK(Hex(EncodePublish(Message{"a/b/c", "hello", true})) == Hex(retained));
    // an empty payload is a PUBLISH too (it clears a retained one)
    CHECK(Hex(EncodePublish(Message{"t", "", true})) == "31 03 00 01 74");
}

TEST_CASE("mqtt PUBLISH with a payload over 127 bytes takes two length bytes") {
    const std::string payload(200, 'x');
    const Bytes packet = EncodePublish(Message{"t", payload, false});
    // 3 + 200 = 203: its low 7 bits (0x4B) with the continuation bit, then 1
    CHECK(Hex(Bytes(packet.begin(), packet.begin() + 6)) == "30 cb 01 00 01 74");
    CHECK(packet.size() == 3 + 203);
}

TEST_CASE("mqtt PINGREQ and DISCONNECT") {
    CHECK(Hex(EncodePingReq()) == "c0 00");
    CHECK(Hex(EncodeDisconnect()) == "e0 00");
}

TEST_CASE("mqtt remaining length at the edges of each byte count") {
    CHECK(Hex(Length(0)) == "00");
    CHECK(Hex(Length(127)) == "7f");
    CHECK(Hex(Length(128)) == "80 01");
    CHECK(Hex(Length(16383)) == "ff 7f");
    CHECK(Hex(Length(16384)) == "80 80 01");
    CHECK(Hex(Length(2097151)) == "ff ff 7f");
    CHECK(Hex(Length(2097152)) == "80 80 80 01");
    CHECK(Hex(Length(268435455)) == "ff ff ff 7f");
}

TEST_CASE("mqtt remaining lengths read back through PacketReader") {
    for (uint32_t length : {0u, 127u, 128u, 16383u, 16384u, 2097152u}) {
        CAPTURE(length);
        Bytes packet{0xD0};
        AppendRemainingLength(packet, length);
        const size_t header = packet.size();
        packet.resize(header + length, 0x5A);
        PacketReader reader;
        // all but the last byte: not whole yet
        reader.Add(packet.data(), packet.size() - 1);
        CHECK_FALSE(reader.Next().has_value());
        reader.Add(packet.data() + packet.size() - 1, 1);
        const auto got = reader.Next();
        REQUIRE(got.has_value());
        CHECK(got->type == kPingResp);
        CHECK(got->flags == 0);
        CHECK(got->body.size() == length);
        CHECK_FALSE(reader.Bad());
        CHECK_FALSE(reader.Next().has_value());
    }
}

TEST_CASE("mqtt PacketReader joins a packet split over several pieces") {
    const Bytes packet = Join({{0x31, 7}, {0x00, 0x01}, Ascii("t"), Ascii("abcd")});
    PacketReader reader;
    for (size_t i = 0; i < packet.size(); i++) {
        CHECK_FALSE(reader.Next().has_value());
        reader.Add(&packet[i], 1);
    }
    const auto got = reader.Next();
    REQUIRE(got.has_value());
    CHECK(got->type == kPublish);
    CHECK(got->flags == 1);
    CHECK(Hex(got->body) == Hex(Join({{0x00, 0x01}, Ascii("t"), Ascii("abcd")})));
    CHECK_FALSE(reader.Next().has_value());
}

TEST_CASE("mqtt PacketReader splits two packets that came together") {
    const Bytes both{0x20, 0x02, 0x00, 0x00, 0xD0, 0x00};
    PacketReader reader;
    reader.Add(both.data(), both.size());
    const auto first = reader.Next();
    REQUIRE(first.has_value());
    CHECK(first->type == kConnack);
    CHECK(ConnackCode(*first) == 0);
    const auto second = reader.Next();
    REQUIRE(second.has_value());
    CHECK(second->type == kPingResp);
    CHECK(second->body.empty());
    CHECK_FALSE(reader.Next().has_value());
}

TEST_CASE("mqtt PacketReader calls a five-byte length bad") {
    const Bytes bad{0x30, 0x80, 0x80, 0x80, 0x80, 0x01, 0x00};
    PacketReader reader;
    reader.Add(bad.data(), 4);
    CHECK_FALSE(reader.Next().has_value());
    CHECK_FALSE(reader.Bad());
    reader.Add(bad.data() + 4, bad.size() - 4);
    CHECK_FALSE(reader.Next().has_value());
    CHECK(reader.Bad());
    // and stays so
    const Bytes good{0xD0, 0x00};
    reader.Add(good.data(), good.size());
    CHECK_FALSE(reader.Next().has_value());
    CHECK(reader.Bad());
}

TEST_CASE("mqtt CONNACK codes and their reasons") {
    for (int code = 0; code <= 5; code++) {
        CAPTURE(code);
        Packet packet{kConnack, 0, {0x00, static_cast<uint8_t>(code)}};
        CHECK(ConnackCode(packet) == code);
    }
    // session present doesn't change the code
    CHECK(ConnackCode(Packet{kConnack, 0, {0x01, 0x00}}) == 0);
    CHECK(ConnackCode(Packet{kConnack, 0, {0x00}}) == -1);
    CHECK(ConnackCode(Packet{kConnack, 0, {}}) == -1);
    CHECK(ConnackCode(Packet{kConnack, 0, {0x00, 0x00, 0x00}}) == -1);
    CHECK(ConnackCode(Packet{kPingResp, 0, {0x00, 0x00}}) == -1);

    CHECK(ConnackText(1) == "unacceptable protocol version");
    CHECK(ConnackText(2) == "identifier rejected");
    CHECK(ConnackText(3) == "server unavailable");
    CHECK(ConnackText(4) == "bad user name or password");
    CHECK(ConnackText(5) == "not authorized");
    CHECK(ConnackText(6) == "refused (code 6)");
    CHECK(ConnackText(-1) == "refused (code -1)");
}
