// Checks the MQTT client (src/Net/mqtt_client.cpp): its session on a fake
// clock, and the client itself over this machine's loopback, against a
// scripted broker.

#include <doctest/doctest.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "src/Net/mqtt_client.h"
#include "src/Net/native_socket.h"
#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace band3::mqtt;
using namespace std::chrono_literals;

namespace {

using Clock = Session::Clock;
using State = Session::State;

const Clock::time_point kT0 = Clock::time_point{} + 1h;

std::string Hex(const Bytes& bytes) {
    std::string text;
    char part[4];
    for (size_t i = 0; i < bytes.size(); i++) {
        std::snprintf(part, sizeof(part), i ? " %02x" : "%02x", bytes[i]);
        text += part;
    }
    return text;
}

ConnectOptions Options() {
    ConnectOptions options;
    options.client_id = "band3_pc";
    options.will = Will{"band3/pc/status", "offline", true};
    options.keepalive_s = 60;
    return options;
}

void Receive(Session& session, const Bytes& bytes, Clock::time_point now) {
    session.Received(bytes.data(), bytes.size(), now);
}

const Bytes kAccepted{0x20, 0x02, 0x00, 0x00};
const Bytes kPong{0xD0, 0x00};

// a session the broker accepted at kT0
Session Accepted() {
    Session session(Options());
    session.Connected(kT0);
    Receive(session, kAccepted, kT0);
    return session;
}

}  // namespace

TEST_CASE("mqtt session: an accepted CONNACK connects it, and only then does it publish") {
    Session session(Options());
    const Message message{"band3/pc/song", "Song", true};
    CHECK(session.Publish(message).empty());
    CHECK(Hex(session.Connected(kT0)) == Hex(EncodeConnect(Options())));
    CHECK(session.state() == State::kConnecting);
    CHECK(session.Publish(message).empty());
    CHECK_FALSE(session.WasConnected());
    Receive(session, kAccepted, kT0 + 100ms);
    CHECK(session.state() == State::kConnected);
    CHECK(session.WasConnected());
    CHECK_FALSE(session.Over());
    CHECK(session.error().empty());
    CHECK(Hex(session.Publish(message)) == Hex(EncodePublish(message)));
}

TEST_CASE("mqtt session: a CONNACK in pieces counts once it's whole") {
    Session session(Options());
    session.Connected(kT0);
    Receive(session, {0x20}, kT0);
    Receive(session, {0x02, 0x00}, kT0);
    CHECK(session.state() == State::kConnecting);
    Receive(session, {0x00}, kT0);
    CHECK(session.state() == State::kConnected);
}

TEST_CASE("mqtt session: a bad user name or password isn't retried, server unavailable is") {
    Session refused(Options());
    refused.Connected(kT0);
    Receive(refused, {0x20, 0x02, 0x00, 0x04}, kT0);
    CHECK(refused.state() == State::kRefused);
    CHECK(refused.error() == "bad user name or password");
    CHECK(refused.Over());
    CHECK_FALSE(refused.Retry());
    CHECK_FALSE(refused.WasConnected());
    CHECK(refused.Publish(Message{"t", "p", false}).empty());

    Session unauthorized(Options());
    unauthorized.Connected(kT0);
    Receive(unauthorized, {0x20, 0x02, 0x00, 0x05}, kT0);
    CHECK(unauthorized.error() == "not authorized");
    CHECK_FALSE(unauthorized.Retry());

    for (int code : {1, 2, 3}) {
        CAPTURE(code);
        Session busy(Options());
        busy.Connected(kT0);
        Receive(busy, {0x20, 0x02, 0x00, static_cast<uint8_t>(code)}, kT0);
        CHECK(busy.state() == State::kRefused);
        CHECK(busy.error() == ConnackText(code));
        CHECK(busy.Retry());
    }

    Session unknown(Options());
    unknown.Connected(kT0);
    Receive(unknown, {0x20, 0x02, 0x00, 0x09}, kT0);
    CHECK(unknown.error() == "refused (code 9)");
    CHECK_FALSE(unknown.Retry());
}

TEST_CASE("mqtt session: no CONNACK within 5 s loses the connection") {
    Session session(Options());
    // the wait starts with CONNECT, not before
    CHECK(session.Tick(kT0).empty());
    session.Connected(kT0);
    CHECK(session.Tick(kT0 + 5s).empty());
    CHECK(session.state() == State::kConnecting);
    CHECK(session.Tick(kT0 + 5s + 1ms).empty());
    CHECK(session.state() == State::kLost);
    CHECK(session.error() == "no answer from the broker");
    CHECK(session.Retry());
}

TEST_CASE("mqtt session: a PINGREQ every half keepalive, lost without a PINGRESP") {
    Session session = Accepted();
    CHECK(session.Tick(kT0 + 29s).empty());
    CHECK(Hex(session.Tick(kT0 + 30s)) == "c0 00");
    // one at a time
    CHECK(session.Tick(kT0 + 31s).empty());
    CHECK(session.Tick(kT0 + 60s).empty());
    CHECK(session.state() == State::kConnected);
    CHECK(session.Tick(kT0 + 60s + 1ms).empty());
    CHECK(session.state() == State::kLost);
    CHECK(session.error() == "the broker stopped answering");
    CHECK(session.Retry());
    CHECK(session.WasConnected());
}

TEST_CASE("mqtt session: a PINGRESP keeps it connected") {
    Session session = Accepted();
    CHECK(Hex(session.Tick(kT0 + 30s)) == "c0 00");
    Receive(session, kPong, kT0 + 31s);
    CHECK(session.Tick(kT0 + 59s).empty());
    CHECK(Hex(session.Tick(kT0 + 60s)) == "c0 00");
    Receive(session, kPong, kT0 + 61s);
    CHECK(session.Tick(kT0 + 89s).empty());
    CHECK(session.state() == State::kConnected);
}

TEST_CASE("mqtt session: packets it didn't ask for are stepped over") {
    Session session = Accepted();
    // a PUBLISH and a SUBACK, together, then a second CONNACK
    Receive(session, {0x30, 0x04, 0x00, 0x01, 't', 'x', 0x90, 0x03, 0x00, 0x01, 0x00}, kT0);
    Receive(session, {0x20, 0x02, 0x00, 0x05}, kT0);
    CHECK(session.state() == State::kConnected);
    CHECK(session.error().empty());
}

TEST_CASE("mqtt session: bad data ends the connection") {
    Session session = Accepted();
    Receive(session, {0x30, 0x80, 0x80, 0x80, 0x80, 0x01}, kT0);
    CHECK(session.state() == State::kLost);
    CHECK(session.error() == "bad data from the broker");
    CHECK(session.Retry());

    Session short_connack(Options());
    short_connack.Connected(kT0);
    Receive(short_connack, {0x20, 0x01, 0x00}, kT0);
    CHECK(short_connack.state() == State::kLost);
    CHECK(short_connack.error() == "bad data from the broker");
}

TEST_CASE("mqtt session: a dropped or failed connection is worth another try") {
    Session session = Accepted();
    session.Closed();
    CHECK(session.state() == State::kLost);
    CHECK(session.error() == "the broker closed the connection");
    CHECK(session.Retry());
    CHECK(session.WasConnected());
    // what comes after changes nothing
    Receive(session, kAccepted, kT0);
    CHECK(session.state() == State::kLost);

    Session never(Options());
    never.Failed("no IPv4 address for broker");
    CHECK(never.state() == State::kLost);
    CHECK(never.error() == "no IPv4 address for broker");
    CHECK(never.Retry());
    CHECK_FALSE(never.WasConnected());
}

TEST_CASE("mqtt status text") {
    CHECK(StatusText({}) == "off");
    CHECK(StatusText({ClientStatus::State::kConnecting, "", 0}) == "connecting");
    CHECK(StatusText({ClientStatus::State::kConnected, "", 0}) == "connected");
    CHECK(StatusText({ClientStatus::State::kRetrying, "x refused the connection", 5}) == "retrying in 5 s");
    CHECK(StatusText({ClientStatus::State::kRefused, "not authorized", 0}) == "refused: not authorized");
}

namespace {

#ifdef _WIN32
using Handle = SOCKET;
constexpr Handle kNone = INVALID_SOCKET;
void Close(Handle s) { closesocket(s); }
#else
using Handle = int;
constexpr Handle kNone = -1;
void Close(Handle s) { close(s); }
#endif

// a TCP listener on 127.0.0.1, on a port the system picks
Handle Listen(uint16_t& port) {
    REQUIRE_FALSE(band3::net::ResolveIPv4("127.0.0.1").empty());  // starts Winsock
    const Handle s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) == 0);
    REQUIRE(listen(s, 4) == 0);
    uint32_t address = 0;
    REQUIRE(band3::net::BoundAddress(s, port, address));
    port = ntohs(port);
    return s;
}

// whether a connection comes in on `listener` within `wait`
bool Incoming(Handle listener, std::chrono::milliseconds wait) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(listener, &set);
    timeval timeout{static_cast<long>(wait.count() / 1000), static_cast<long>((wait.count() % 1000) * 1000)};
#ifdef _WIN32
    const int nfds = 0;  // ignored on Windows
#else
    const int nfds = listener + 1;
#endif
    return select(nfds, &set, nullptr, nullptr, &timeout) > 0;
}

// What a scripted broker heard, from its thread.
struct Heard {
    std::mutex mutex;
    std::vector<Packet> packets;

    void Add(Packet packet) {
        std::lock_guard<std::mutex> lock(mutex);
        packets.push_back(std::move(packet));
    }
    std::vector<Packet> Get() {
        std::lock_guard<std::mutex> lock(mutex);
        return packets;
    }
};

// One connection of the broker: records what comes in, answers CONNECT with
// `connack` (nothing if empty) and PINGREQ with PINGRESP, and closes after
// `packets` packets, or when the client closes its end.
void Serve(Handle s, Heard& heard, const Bytes& connack, size_t packets = SIZE_MAX) {
    PacketReader reader;
    uint8_t buffer[512];
    size_t count = 0;
    while (count < packets) {
        const int n = static_cast<int>(recv(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
        if (n <= 0) break;
        reader.Add(buffer, static_cast<size_t>(n));
        while (count < packets) {
            auto packet = reader.Next();
            if (!packet) break;
            count++;
            if (packet->type == kConnect && !connack.empty()) {
                send(s, reinterpret_cast<const char*>(connack.data()), static_cast<int>(connack.size()), 0);
            } else if (packet->type == kPingReq) {
                send(s, reinterpret_cast<const char*>(kPong.data()), static_cast<int>(kPong.size()), 0);
            }
            heard.Add(std::move(*packet));
        }
    }
    Close(s);
}

// a PUBLISH's topic and payload
Message Published(const Packet& packet) {
    Message message;
    if (packet.type != kPublish || packet.body.size() < 2) return message;
    const size_t length = (static_cast<size_t>(packet.body[0]) << 8) | packet.body[1];
    message.topic.assign(packet.body.begin() + 2, packet.body.begin() + 2 + static_cast<std::ptrdiff_t>(length));
    message.payload.assign(packet.body.begin() + 2 + static_cast<std::ptrdiff_t>(length), packet.body.end());
    message.retain = (packet.flags & 1) != 0;
    return message;
}

// polls `done` until it holds, for up to 5 s
template <typename F>
bool WaitUntil(F done) {
    const auto deadline = Clock::now() + 5s;
    while (!done()) {
        if (Clock::now() >= deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
    return true;
}

// the log lines a client wrote, from its thread
struct Lines {
    std::mutex mutex;
    std::vector<std::string> lines;

    std::function<void(const std::string&)> Sink() {
        return [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(mutex);
            lines.push_back(line);
        };
    }
    std::vector<std::string> Get() {
        std::lock_guard<std::mutex> lock(mutex);
        return lines;
    }
};

ClientConfig LoopbackConfig(uint16_t port) {
    ClientConfig config;
    config.host = "127.0.0.1";
    config.port = port;
    config.connect = Options();
    config.connect.username = "ha";
    config.connect.password = "hunter2";
    return config;
}

}  // namespace

TEST_CASE("mqtt client: publishes what it produces, then the last messages and DISCONNECT on Stop") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Heard heard;
    std::thread broker([&] {
        const Handle s = accept(listener, nullptr, nullptr);
        if (s != kNone) Serve(s, heard, kAccepted);
    });
    Lines log;
    ClientConfig config = LoopbackConfig(port);
    config.log = log.Sink();
    std::atomic<int> calls{0}, fresh_calls{0};
    Client client;
    client.Start(config, [&](bool fresh) {
        calls++;
        if (!fresh) return std::vector<Message>{};
        fresh_calls++;
        return std::vector<Message>{{"band3/pc/song", "Song", true}};
    });
    CHECK(WaitUntil([&] { return client.GetStatus().state == ClientStatus::State::kConnected; }));
    // it goes on calling, every 50 ms or so
    CHECK(WaitUntil([&] { return calls >= 3; }));
    CHECK(WaitUntil([&] { return heard.Get().size() >= 2; }));
    client.Stop({{"band3/pc/status", "offline", true}});
    broker.join();
    Close(listener);

    CHECK(client.GetStatus().state == ClientStatus::State::kOff);
    CHECK(fresh_calls == 1);
    const std::vector<Packet> packets = heard.Get();
    REQUIRE(packets.size() == 4);
    CHECK(packets[0].type == kConnect);
    const Bytes connect = EncodeConnect(config.connect);
    CHECK(Hex(packets[0].body) == Hex(Bytes(connect.begin() + 2, connect.end())));
    CHECK(packets[1].type == kPublish);
    CHECK(Published(packets[1]).topic == "band3/pc/song");
    CHECK(Published(packets[1]).payload == "Song");
    CHECK(Published(packets[1]).retain);
    CHECK(packets[2].type == kPublish);
    CHECK(Published(packets[2]).topic == "band3/pc/status");
    CHECK(Published(packets[2]).payload == "offline");
    CHECK(packets[3].type == kDisconnect);

    const std::string where = "127.0.0.1:" + std::to_string(port);
    const std::vector<std::string> lines = log.Get();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0] == "connecting to " + where);
    CHECK(lines[1] == "connected to " + where);
}

TEST_CASE("mqtt client: a refused login stays refused, without connecting again") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Heard heard;
    bool again = true;
    std::thread broker([&] {
        const Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        Serve(s, heard, {0x20, 0x02, 0x00, 0x04});
        again = Incoming(listener, 500ms);
    });
    Lines log;
    ClientConfig config = LoopbackConfig(port);
    config.log = log.Sink();
    config.retry_delays = {50ms};
    Client client;
    client.Start(config, [](bool) { return std::vector<Message>{{"t", "p", false}}; });
    broker.join();
    Close(listener);
    CHECK_FALSE(again);
    ClientStatus status = client.GetStatus();
    CHECK(status.state == ClientStatus::State::kRefused);
    CHECK(status.error == "bad user name or password");
    CHECK(StatusText(status) == "refused: bad user name or password");
    // only the CONNECT went
    CHECK(heard.Get().size() == 1);
    client.Stop();
    CHECK(client.GetStatus().state == ClientStatus::State::kRefused);

    const std::vector<std::string> lines = log.Get();
    REQUIRE(lines.size() == 2);
    CHECK(lines[1] == "bad user name or password");
    for (const std::string& line : lines) CHECK(line.find("hunter2") == std::string::npos);
}

TEST_CASE("mqtt client: reconnects after a drop, resends fresh, and logs each failure once in a row") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Heard heard;
    std::thread broker([&] {
        // twice: hears CONNECT and hangs up
        for (int i = 0; i < 2; i++) {
            const Handle s = accept(listener, nullptr, nullptr);
            if (s == kNone) return;
            Serve(s, heard, {}, 1);
        }
        // accepts, hears the fresh PUBLISH, hangs up
        Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        Serve(s, heard, kAccepted, 2);
        // accepts, until the client closes
        s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        Serve(s, heard, kAccepted);
    });
    Lines log;
    ClientConfig config = LoopbackConfig(port);
    config.log = log.Sink();
    config.retry_delays = {0ms, 20ms};
    std::atomic<int> fresh_calls{0};
    Client client;
    client.Start(config, [&](bool fresh) {
        if (!fresh) return std::vector<Message>{};
        fresh_calls++;
        return std::vector<Message>{{"band3/pc/song", "Song", true}};
    });
    CHECK(WaitUntil([&] { return fresh_calls == 2 && heard.Get().size() >= 6; }));
    CHECK(client.GetStatus().state == ClientStatus::State::kConnected);
    client.Stop();
    broker.join();
    Close(listener);

    const std::vector<Packet> packets = heard.Get();
    // CONNECT, CONNECT, CONNECT + PUBLISH, CONNECT + PUBLISH + DISCONNECT
    REQUIRE(packets.size() == 7);
    CHECK(packets[4].type == kConnect);
    CHECK(Published(packets[5]).topic == "band3/pc/song");
    CHECK(packets[6].type == kDisconnect);

    // the drop after connecting came long before stable_after, so it's the
    // same failure as before and isn't news, nor is connecting again
    const std::string where = "127.0.0.1:" + std::to_string(port);
    const std::vector<std::string> lines = log.Get();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0] == "connecting to " + where);
    CHECK(lines[1] == "the broker closed the connection");
    CHECK(lines[2] == "connected to " + where);
}

TEST_CASE("mqtt client: a connection that lasted starts the waits over and logs its drop") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Heard heard;
    std::atomic<int> connections{0};
    std::thread broker([&] {
        // hears CONNECT and hangs up, twice, then accepts and hangs up after
        // the fresh PUBLISH, then accepts until the client closes
        for (int i = 0; i < 4; i++) {
            const Handle s = accept(listener, nullptr, nullptr);
            if (s == kNone) return;
            connections++;
            if (i < 2) {
                Serve(s, heard, {}, 1);
            } else if (i == 2) {
                Serve(s, heard, kAccepted, 2);
            } else {
                Serve(s, heard, kAccepted);
            }
        }
    });
    Lines log;
    ClientConfig config = LoopbackConfig(port);
    config.log = log.Sink();
    // were the waits not started over, the last connection would come 10 s late
    config.retry_delays = {0ms, 20ms, 10s};
    config.stable_after = 50ms;
    std::atomic<int> fresh_calls{0};
    Client client;
    client.Start(config, [&](bool fresh) {
        if (!fresh) return std::vector<Message>{};
        fresh_calls++;
        // the third connection holds on to its PUBLISH a while, so it's
        // connected for longer than stable_after before the broker hangs up
        if (fresh_calls == 1) std::this_thread::sleep_for(100ms);
        return std::vector<Message>{{"band3/pc/song", "Song", true}};
    });
    CHECK(WaitUntil([&] { return fresh_calls == 2 && client.GetStatus().state == ClientStatus::State::kConnected; }));
    client.Stop();
    broker.join();
    Close(listener);
    CHECK(connections == 4);

    const std::string where = "127.0.0.1:" + std::to_string(port);
    const std::vector<std::string> lines = log.Get();
    REQUIRE(lines.size() == 5);
    CHECK(lines[0] == "connecting to " + where);
    CHECK(lines[1] == "the broker closed the connection");
    CHECK(lines[2] == "connected to " + where);
    CHECK(lines[3] == "the broker closed the connection");
    CHECK(lines[4] == "connected to " + where);
}

TEST_CASE("mqtt client: a broker that accepts and drops at once is backed off from, and logged once") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Heard heard;
    std::atomic<bool> done{false};
    std::atomic<int> connections{0};
    std::thread broker([&] {
        // every connection: CONNACK, then hang up (as when another client
        // takes the id over)
        while (!done) {
            if (!Incoming(listener, 20ms)) continue;
            const Handle s = accept(listener, nullptr, nullptr);
            if (s == kNone) return;
            connections++;
            Serve(s, heard, kAccepted, 1);
        }
    });
    Lines log;
    ClientConfig config = LoopbackConfig(port);
    config.log = log.Sink();
    config.retry_delays = {0ms, 200ms, 400ms, 800ms, 1600ms};
    Client client;
    // nothing published, so the broker's close is a plain one, never a reset
    client.Start(config, nullptr);
    std::this_thread::sleep_for(1s);
    client.Stop();
    done = true;
    broker.join();
    Close(listener);

    // at 0, at once, then 200 and 400 ms later; the next would be at 1.4 s
    CHECK(connections >= 2);
    CHECK(connections <= 4);
    // the first drop is news and so is the connection after it; the same drop
    // again isn't, nor is connecting again after it
    const std::string where = "127.0.0.1:" + std::to_string(port);
    const std::vector<std::string> lines = log.Get();
    REQUIRE(lines.size() == 4);
    CHECK(lines[0] == "connecting to " + where);
    CHECK(lines[1] == "connected to " + where);
    CHECK(lines[2] == "the broker closed the connection");
    CHECK(lines[3] == "connected to " + where);
}

TEST_CASE("mqtt client: Stop doesn't wait for a slow name lookup") {
    // shared with the lookup's thread, which outlives the client
    auto lookups = std::make_shared<std::atomic<int>>(0);
    ClientConfig config = LoopbackConfig(1883);
    config.host = "broker.invalid";
    config.resolve = [lookups](const std::string&) {
        (*lookups)++;
        std::this_thread::sleep_for(2s);
        return std::vector<uint32_t>{};
    };
    Client client;
    client.Start(config, nullptr);
    CHECK(WaitUntil([&] { return *lookups == 1; }));
    CHECK(client.GetStatus().state == ClientStatus::State::kConnecting);
    const auto stopping = Clock::now();
    client.Stop();
    CHECK(Clock::now() - stopping < 500ms);
    CHECK(client.GetStatus().state == ClientStatus::State::kOff);
}

TEST_CASE("mqtt client: nothing listening is retried, and Stop ends the wait at once") {
    uint16_t port = 0;
    Close(Listen(port));
    ClientConfig config = LoopbackConfig(port);
    config.retry_delays = {5s};
    Client client;
    client.Start(config, nullptr);
    CHECK(WaitUntil([&] { return client.GetStatus().state == ClientStatus::State::kRetrying; }));
    const ClientStatus status = client.GetStatus();
    CHECK(status.error == "127.0.0.1:" + std::to_string(port) + " refused the connection");
    CHECK(status.retry_in_s >= 4);
    CHECK(status.retry_in_s <= 5);
    const auto stopping = Clock::now();
    client.Stop();
    CHECK(Clock::now() - stopping < 1s);
    CHECK(client.GetStatus().state == ClientStatus::State::kOff);
}

TEST_CASE("mqtt client: Stop without Start, and twice") {
    Client client;
    client.Stop();
    client.Stop({{"t", "p", true}});
    CHECK(client.GetStatus().state == ClientStatus::State::kOff);
}
