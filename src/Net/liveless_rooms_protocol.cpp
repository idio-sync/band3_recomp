#include "liveless_rooms_protocol.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include "sha1.h"

namespace band3::rooms {

namespace {

// Bodies' sizes, as RB3Enhanced packs them
constexpr size_t kClientHelloSize = 53;
constexpr size_t kClientLoginSize = 28 + kProofSize;
constexpr size_t kJoinRequestSize = 8;
constexpr size_t kServerHelloSize = 18;
constexpr size_t kServerLoggedInSize = 12;
constexpr size_t kJoinResponseSize = 33;
constexpr size_t kJoinDeniedSize = 1;
constexpr size_t kNatPunchRequestSize = 4;

constexpr size_t kGamertagField = 16, kVersionField = 0x30, kLanguageField = 3, kCodeField = 8;

// Lays out one packet: its header, then its body's fields in order.
class Writer {
public:
    Writer(uint8_t type, size_t body_size) {
        bytes_.reserve(kHeaderSize + body_size);
        Be(kMagic, 2);
        U8(0);
        U8(type);
        Be(body_size, 2);
    }

    void U8(uint8_t value) { bytes_.push_back(value); }
    void Be(uint64_t value, int size) {
        for (int i = size - 1; i >= 0; i--) bytes_.push_back(static_cast<uint8_t>(value >> (i * 8)));
    }
    // already in network order: its bytes as they lie
    void Ipv4(uint32_t address) { Raw(&address, 4); }
    void Raw(const void* data, size_t size) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        bytes_.insert(bytes_.end(), bytes, bytes + size);
    }
    // `text`, cut to `max_chars`, then NULs to the field's end
    void Text(std::string_view text, size_t field, size_t max_chars) {
        text = text.substr(0, std::min(max_chars, field));
        Raw(text.data(), text.size());
        bytes_.resize(bytes_.size() + field - text.size(), 0);
    }
    void Zeros(size_t size) { bytes_.resize(bytes_.size() + size, 0); }

    Bytes Done() { return std::move(bytes_); }

private:
    Bytes bytes_;
};

// Reads a body's fields in order; the caller checks the body is long enough.
class Reader {
public:
    explicit Reader(const Bytes& body) : body_(body) {}

    uint8_t U8() { return body_[at_++]; }
    uint64_t Be(int size) {
        uint64_t value = 0;
        for (int i = 0; i < size; i++) value = (value << 8) | body_[at_++];
        return value;
    }
    uint32_t Ipv4() {
        uint32_t address;
        std::memcpy(&address, body_.data() + at_, 4);
        at_ += 4;
        return address;
    }
    template <size_t N>
    std::array<uint8_t, N> Array() {
        std::array<uint8_t, N> bytes;
        std::memcpy(bytes.data(), body_.data() + at_, N);
        at_ += N;
        return bytes;
    }
    // up to the first NUL or the field's end
    std::string Text(size_t field) {
        const char* start = reinterpret_cast<const char*>(body_.data() + at_);
        at_ += field;
        return std::string(start, std::find(start, start + field, '\0'));
    }

private:
    const Bytes& body_;
    size_t at_ = 0;
};

}  // namespace

Bytes Encode(const ClientHello& hello) {
    Writer w(static_cast<uint8_t>(ClientType::Hello), kClientHelloSize);
    w.U8(kProtocolVersion);
    w.U8(hello.emulator ? 1 : 0);
    w.Text(hello.language, kLanguageField, kLanguageField);
    w.Text(hello.version, kVersionField, kVersionField - 1);
    return w.Done();
}

Bytes Encode(const ClientLogin& login) {
    Writer w(static_cast<uint8_t>(ClientType::Login), kClientLoginSize);
    w.Be(login.xuid, 8);
    w.Text(login.gamertag, kGamertagField, kGamertagField - 1);
    w.Ipv4(login.local_ipv4);
    // as RB3Enhanced on Xenia sends it: the hash where a console's signature
    // would start
    if (login.proof) {
        w.Raw(login.proof->data(), login.proof->size());
        w.Zeros(kProofSize - login.proof->size());
    } else {
        w.Zeros(kProofSize);
    }
    return w.Done();
}

Bytes Encode(const Pong&) { return Writer(static_cast<uint8_t>(ClientType::Pong), 0).Done(); }

Bytes Encode(const JoinRequest& request) {
    Writer w(static_cast<uint8_t>(ClientType::JoinRequest), kJoinRequestSize);
    w.Text(request.code, kCodeField, kCodeField);
    return w.Done();
}

Bytes Encode(const ServerHello& hello) {
    Writer w(static_cast<uint8_t>(ServerType::Hello), kServerHelloSize);
    w.U8(hello.allowed ? 1 : 0);
    w.U8(hello.needs_proof ? 1 : 0);
    w.Raw(hello.proof_key.data(), hello.proof_key.size());
    return w.Done();
}

Bytes Encode(const ServerLoggedIn& logged_in) {
    Writer w(static_cast<uint8_t>(ServerType::LoggedIn), kServerLoggedInSize);
    w.Ipv4(logged_in.public_ipv4);
    w.Text(logged_in.code, kCodeField, kCodeField);
    return w.Done();
}

Bytes Encode(const Ping&) { return Writer(static_cast<uint8_t>(ServerType::Ping), 0).Done(); }

Bytes Encode(const JoinResponse& response) {
    Writer w(static_cast<uint8_t>(ServerType::JoinResponse), kJoinResponseSize);
    w.U8(response.join_type);
    w.Text(response.user, kGamertagField, kGamertagField - 1);
    w.Be(response.xuid, 8);
    w.Ipv4(response.public_ipv4);
    w.Ipv4(response.private_ipv4);
    return w.Done();
}

Bytes Encode(const JoinDenied& denied) {
    Writer w(static_cast<uint8_t>(ServerType::JoinDenied), kJoinDeniedSize);
    w.U8(denied.reason);
    return w.Done();
}

Bytes Encode(const NatPunchRequest& request) {
    Writer w(static_cast<uint8_t>(ServerType::NatPunchRequest), kNatPunchRequestSize);
    w.Ipv4(request.public_ipv4);
    return w.Done();
}

void FrameReader::Feed(const uint8_t* data, size_t size) {
    if (broken_) return;
    buffer_.insert(buffer_.end(), data, data + size);
}

std::optional<Frame> FrameReader::Next() {
    if (broken_ || buffer_.size() < kHeaderSize) return std::nullopt;
    const uint16_t magic = static_cast<uint16_t>((buffer_[0] << 8) | buffer_[1]);
    const size_t body_size = (size_t{buffer_[4]} << 8) | buffer_[5];
    if (magic != kMagic || body_size > kMaxBody) {
        broken_ = true;
        buffer_.clear();
        return std::nullopt;
    }
    if (buffer_.size() < kHeaderSize + body_size) return std::nullopt;

    Frame frame{buffer_[3], Bytes(buffer_.begin() + kHeaderSize, buffer_.begin() + kHeaderSize + body_size)};
    buffer_.erase(buffer_.begin(), buffer_.begin() + kHeaderSize + body_size);
    return frame;
}

std::optional<ServerMessage> DecodeServer(const Frame& frame) {
    Reader r(frame.body);
    const size_t size = frame.body.size();
    switch (static_cast<ServerType>(frame.type)) {
    case ServerType::Hello: {
        if (size < kServerHelloSize) return std::nullopt;
        ServerHello hello;
        hello.allowed = r.U8() != 0;
        hello.needs_proof = r.U8() != 0;
        hello.proof_key = r.Array<16>();
        return hello;
    }
    case ServerType::LoggedIn: {
        if (size < kServerLoggedInSize) return std::nullopt;
        ServerLoggedIn logged_in;
        logged_in.public_ipv4 = r.Ipv4();
        logged_in.code = r.Text(kCodeField);
        return logged_in;
    }
    case ServerType::Ping:
        return Ping{};
    case ServerType::JoinResponse: {
        if (size < kJoinResponseSize) return std::nullopt;
        JoinResponse response;
        response.join_type = r.U8();
        response.user = r.Text(kGamertagField);
        response.xuid = r.Be(8);
        response.public_ipv4 = r.Ipv4();
        response.private_ipv4 = r.Ipv4();
        return response;
    }
    case ServerType::JoinDenied:
        if (size < kJoinDeniedSize) return std::nullopt;
        return JoinDenied{r.U8()};
    case ServerType::NatPunchRequest:
        if (size < kNatPunchRequestSize) return std::nullopt;
        return NatPunchRequest{r.Ipv4()};
    }
    return Unknown{frame.type};
}

std::optional<ClientMessage> DecodeClient(const Frame& frame) {
    Reader r(frame.body);
    const size_t size = frame.body.size();
    switch (static_cast<ClientType>(frame.type)) {
    case ClientType::Hello: {
        if (size < kClientHelloSize) return std::nullopt;
        r.U8();  // protocol version, only ever 0
        ClientHello hello;
        hello.emulator = r.U8() != 0;
        hello.language = r.Text(kLanguageField);
        hello.version = r.Text(kVersionField);
        return hello;
    }
    case ClientType::Login: {
        if (size < kClientLoginSize) return std::nullopt;
        ClientLogin login;
        login.xuid = r.Be(8);
        login.gamertag = r.Text(kGamertagField);
        login.local_ipv4 = r.Ipv4();
        const auto proof = r.Array<kProofSize>();
        if (std::any_of(proof.begin(), proof.end(), [](uint8_t b) { return b != 0; })) {
            login.proof.emplace();
            std::copy_n(proof.begin(), login.proof->size(), login.proof->begin());
        }
        return login;
    }
    case ClientType::Pong:
        return Pong{};
    case ClientType::JoinRequest:
        if (size < kJoinRequestSize) return std::nullopt;
        return JoinRequest{r.Text(kCodeField)};
    }
    return Unknown{frame.type};
}

std::array<uint8_t, 20> LoginProof(const std::array<uint8_t, 16>& key, std::string_view server_address,
                                   uint64_t xuid) {
    Bytes message(server_address.begin(), server_address.end());
    for (int i = 7; i >= 0; i--) message.push_back(static_cast<uint8_t>(xuid >> (i * 8)));
    return net::HmacSha1(key, message);
}

uint64_t RoomsXuid(std::string_view username) {
    uint64_t hash = 0xCBF29CE484222325;
    for (char c : username) {
        hash ^= static_cast<uint8_t>(c);
        hash *= 0x100000001B3;
    }
    constexpr uint64_t kLow48 = 0x0000FFFFFFFFFFFF;
    const uint64_t low = (hash & kLow48) ? hash & kLow48 : 1;
    return 0x0009000000000000 | low;
}

}
