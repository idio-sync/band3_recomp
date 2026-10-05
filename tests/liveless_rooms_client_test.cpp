// Checks the Liveless Rooms client (src/Net/liveless_rooms_client.cpp): its
// session, packet by packet against tests/golden's vectors (the bytes
// RB3Enhanced sends, which the mock server is held to too), and the client
// itself over this machine's loopback, against a scripted server.

#include <doctest/doctest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "src/Net/liveless_rooms_client.h"
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

using namespace band3::rooms;
using namespace std::chrono_literals;

namespace {

using Clock = Session::Clock;

// the golden file's inputs
constexpr uint32_t kLoopback = 0x0100007F;  // 127.0.0.1, network order
constexpr uint32_t kLan = 0x0201A8C0;       // 192.168.1.2
constexpr uint32_t kElsewhere = 0x0A0A0A0A; // 10.10.10.10

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

Bytes GoldenBytes(const std::string& name) {
    const auto it = Golden().find(name);
    REQUIRE_MESSAGE(it != Golden().end(), "no golden vector " << name);
    Bytes bytes;
    for (size_t i = 0; i + 1 < it->second.size(); i += 2) {
        bytes.push_back(static_cast<uint8_t>(std::stoi(it->second.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

std::string Hex(const Bytes& bytes) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string hex;
    for (uint8_t b : bytes) {
        hex += kDigits[b >> 4];
        hex += kDigits[b & 15];
    }
    return hex;
}

Config GoldenConfig() {
    Config config;
    config.server = "127.0.0.1";
    config.host = "127.0.0.1";
    config.gamertag = "host";
    config.xuid = 0x0009000000000001ull;
    config.local_ipv4 = kLan;
    config.version = "band3-test";
    config.language = "eng";
    return config;
}

// a session and what it asked of the game
struct Harness {
    std::vector<uint32_t> joins;
    std::vector<uint32_t> punches;
    std::vector<std::string> log;
    std::string join_error;
    Clock::time_point now{};
    Session session;

    explicit Harness(Config config = GoldenConfig())
        : session(std::move(config), Callbacks{
                                         [this](uint32_t ip) {
                                             joins.push_back(ip);
                                             return join_error;
                                         },
                                         [this](uint32_t ip) { punches.push_back(ip); },
                                         [this](const std::string& line) { log.push_back(line); },
                                     }) {}

    Bytes Feed(const Bytes& bytes) { return session.Received(bytes.data(), bytes.size(), now); }
    Bytes Feed(const std::string& golden) { return Feed(GoldenBytes(golden)); }

    // connected and logged in, as the golden packets have it
    void LogIn() {
        session.Connected(now);
        Feed("server_hello");
        Feed("server_logged_in");
        REQUIRE(session.status().state == State::kLoggedIn);
    }

    bool Logged(const std::string& text) const {
        for (const std::string& line : log)
            if (line.find(text) != std::string::npos) return true;
        return false;
    }
};

Bytes JoinResponseFrom(uint32_t public_ipv4, uint32_t private_ipv4) {
    JoinResponse response;
    response.join_type = 0;
    response.user = "host";
    response.xuid = 0x0009000000000001ull;
    response.public_ipv4 = public_ipv4;
    response.private_ipv4 = private_ipv4;
    return Encode(response);
}

}  // namespace

TEST_CASE("a session logs in as RB3Enhanced does, byte for byte") {
    Harness h;
    CHECK(h.session.status().state == State::kOff);
    CHECK(Hex(h.session.Connected(h.now)) == Hex(GoldenBytes("client_hello")));
    CHECK(h.session.status().state == State::kConnected);
    // the proof signs the address as typed, "127.0.0.1", with the XUID
    CHECK(Hex(h.Feed("server_hello")) == Hex(GoldenBytes("client_login")));
    CHECK(h.session.status().state == State::kConnected);
    CHECK(h.Feed("server_logged_in").empty());
    CHECK(h.session.status().state == State::kLoggedIn);
    CHECK(h.session.status().code == "ABCD2345");
    CHECK(h.session.status().public_ipv4 == kLoopback);
    CHECK(h.Logged("rooms: logged in as ABCD2345, public IP 127.0.0.1"));
}

TEST_CASE("a session answers a ping with a pong") {
    Harness h;
    h.LogIn();
    CHECK(Hex(h.Feed("ping")) == Hex(GoldenBytes("pong")));
}

TEST_CASE("a login without a proof asked for sends zeros") {
    Harness h;
    h.session.Connected(h.now);
    ServerHello hello{true, false, {}};
    const Bytes login = h.Feed(Encode(hello));
    REQUIRE(login.size() == kHeaderSize + 28 + kProofSize);
    for (size_t i = kHeaderSize + 28; i < login.size(); i++) CHECK(login[i] == 0);
}

TEST_CASE("packets split anyhow, or run together, read the same") {
    Harness h;
    h.session.Connected(h.now);
    Bytes login;
    for (uint8_t byte : GoldenBytes("server_hello")) {
        const Bytes out = h.Feed(Bytes{byte});
        login.insert(login.end(), out.begin(), out.end());
    }
    CHECK(Hex(login) == Hex(GoldenBytes("client_login")));
    Bytes together = GoldenBytes("server_logged_in");
    const Bytes ping = GoldenBytes("ping");
    together.insert(together.end(), ping.begin(), ping.end());
    together.insert(together.end(), ping.begin(), ping.end());
    CHECK(Hex(h.Feed(together)) == Hex(GoldenBytes("pong")) + Hex(GoldenBytes("pong")));
    CHECK(h.session.status().code == "ABCD2345");
}

TEST_CASE("a join asks for the code, and goes where the server says the host is") {
    Harness h;
    h.LogIn();
    CHECK(Hex(h.session.Join("ABCD2345")) == Hex(GoldenBytes("join_request")));
    // the host has this player's public address: the same router, so its own
    h.Feed("join_response");
    REQUIRE(h.joins.size() == 1);
    CHECK(h.joins[0] == kLan);
    CHECK(h.session.status().last_join_user == "host");
    CHECK(h.session.status().last_join_ipv4 == kLan);
    CHECK(h.session.status().error.empty());
    CHECK(h.Logged("rooms: joining host at 192.168.1.2"));
}

TEST_CASE("a host elsewhere is joined at its public address") {
    Harness h;
    h.LogIn();
    h.session.Join("ABCD2345");
    h.Feed(JoinResponseFrom(kElsewhere, kLan));
    REQUIRE(h.joins.size() == 1);
    CHECK(h.joins[0] == kElsewhere);
}

TEST_CASE("a join takes whichever address the server has, and fails with none") {
    Harness h;
    h.LogIn();
    // behind the same router, but the server knows no address of its own
    h.Feed(JoinResponseFrom(kLoopback, 0));
    REQUIRE(h.joins.size() == 1);
    CHECK(h.joins[0] == kLoopback);
    h.Feed(JoinResponseFrom(0, 0));
    CHECK(h.joins.size() == 1);
    CHECK(h.session.status().error == "the server gave no address for host");
    CHECK(h.session.status().state == State::kLoggedIn);
}

TEST_CASE("a join the game can't make is the status's error") {
    Harness h;
    h.LogIn();
    h.join_error = "the game isn't running yet";
    h.Feed("join_response");
    CHECK(h.session.status().error == "the game isn't running yet");
    // a new join starts without it
    h.session.Join("ABCD2345");
    CHECK(h.session.status().error.empty());
}

TEST_CASE("a code no game has is an error, and the session stays logged in") {
    Harness h;
    h.LogIn();
    h.session.Join("NOPE0000");
    h.Feed("join_denied");
    CHECK(h.session.status().error == "no game with code NOPE0000");
    CHECK(h.session.status().state == State::kLoggedIn);
    JoinDenied other{3};
    h.Feed(Encode(other));
    CHECK(h.session.status().error == "join denied (reason 3)");
}

TEST_CASE("a join before logging in sends nothing") {
    Harness h;
    CHECK(h.session.Join("ABCD2345").empty());
    h.session.Connected(h.now);
    CHECK(h.session.Join("ABCD2345").empty());
}

TEST_CASE("a NAT punch request goes to the game with the joiner's address") {
    Harness h;
    h.LogIn();
    CHECK(h.Feed("nat_punch").empty());
    REQUIRE(h.punches.size() == 1);
    CHECK(h.punches[0] == kLoopback);
}

TEST_CASE("unknown and short packets are ignored") {
    Harness h;
    h.LogIn();
    const Bytes unknown = {0x4C, 0x4C, 0, 9, 0, 1, 0xAA};
    CHECK(h.Feed(unknown).empty());
    CHECK(h.Logged("unknown type 9"));
    const Bytes short_login = {0x4C, 0x4C, 0, 1, 0, 2, 1, 2};
    h.Feed(short_login);
    CHECK(h.Logged("too short"));
    CHECK(h.session.status().state == State::kLoggedIn);
    CHECK(h.session.status().code == "ABCD2345");
}

TEST_CASE("a server that turns the connection away fails it") {
    Harness h;
    h.session.Connected(h.now);
    ServerHello refused{false, true, {}};
    CHECK(h.Feed(Encode(refused)).empty());
    CHECK(h.session.status().state == State::kFailed);
    CHECK(h.session.status().error == "the server turned the connection away");
    CHECK(h.session.Over());
}

TEST_CASE("a server hanging up fails a login, or disconnects a logged in session") {
    {
        Harness h;
        h.session.Connected(h.now);
        h.Feed("server_hello");
        h.session.Closed();
        CHECK(h.session.status().state == State::kFailed);
        CHECK(h.session.status().error.starts_with("login refused: check that liveless_rooms_server"));
        CHECK(h.session.Over());
    }
    {
        Harness h;
        h.LogIn();
        h.session.Closed();
        CHECK(h.session.status().state == State::kDisconnected);
        CHECK(h.session.status().error == "the server closed the connection");
        // what it had stays
        CHECK(h.session.status().code == "ABCD2345");
    }
}

TEST_CASE("30 s without a word from the server ends the connection") {
    Harness h;
    h.LogIn();
    h.now += 20s;
    h.session.Tick(h.now);
    h.Feed("ping");
    h.now += 29s;
    h.session.Tick(h.now);
    CHECK(h.session.status().state == State::kLoggedIn);
    h.now += 2s;
    h.session.Tick(h.now);
    CHECK(h.session.status().state == State::kDisconnected);
    CHECK(h.session.status().error == "the server stopped answering");

    Harness waiting;
    waiting.session.Connected(waiting.now);
    waiting.now += 31s;
    waiting.session.Tick(waiting.now);
    CHECK(waiting.session.status().state == State::kFailed);
}

TEST_CASE("a stream that isn't Rooms ends the connection") {
    Harness h;
    h.LogIn();
    const Bytes junk = {'H', 'T', 'T', 'P', '/', '1'};
    h.Feed(junk);
    CHECK(h.session.status().state == State::kDisconnected);
    CHECK(h.session.status().error == "bad data from the server");
    // and nothing after it is read
    CHECK(h.Feed("ping").empty());
}

TEST_CASE("a lost connection is worth another try, unless the server said no") {
    {
        // logged in, then gone: the server may be back
        Harness h;
        h.LogIn();
        h.session.Closed();
        CHECK(h.session.Retry());
        Harness silent;
        silent.LogIn();
        silent.now += 31s;
        silent.session.Tick(silent.now);
        CHECK(silent.session.status().state == State::kDisconnected);
        CHECK(silent.session.Retry());
    }
    {
        // no server reached: looking up or connecting failed, or one took the
        // connection and never said a word
        Harness h;
        h.session.Failed("127.0.0.1 refused the connection");
        CHECK(h.session.Retry());
        Harness hung_up;
        hung_up.session.Connected(hung_up.now);
        hung_up.session.Closed();
        CHECK(hung_up.session.status().state == State::kFailed);
        CHECK(hung_up.session.Retry());
        Harness mute;
        mute.session.Connected(mute.now);
        mute.now += 31s;
        mute.session.Tick(mute.now);
        CHECK(mute.session.Retry());
    }
    {
        // the server had its say: asking again gets the same answer
        Harness refused;
        refused.session.Connected(refused.now);
        refused.Feed("server_hello");
        refused.session.Closed();
        CHECK(refused.session.status().error.starts_with("login refused"));
        CHECK_FALSE(refused.session.Retry());
        Harness away;
        away.session.Connected(away.now);
        away.Feed(Encode(ServerHello{false, true, {}}));
        CHECK_FALSE(away.session.Retry());
        Harness junk;
        junk.session.Connected(junk.now);
        junk.Feed(Bytes{'H', 'T', 'T', 'P', '/', '1'});
        CHECK(junk.session.status().error == "bad data from the server");
        CHECK_FALSE(junk.session.Retry());
    }
    // and a session still going isn't over
    Harness going;
    going.LogIn();
    CHECK_FALSE(going.session.Retry());
}

TEST_CASE("a code accepts one to eight letters and digits, in upper case") {
    for (const std::string input : {"a", "lcmee", "host001", "host0001"}) {
        CHECK(IsValidCode(input));
        std::string code = input;
        CHECK(NormalizeCode(code).empty());
        CHECK(code == (input == "a" ? "A" : input == "lcmee" ? "LCMEE" :
                       input == "host001" ? "HOST001" : "HOST0001"));
    }
    for (std::string bad : std::vector<std::string>{"HOST00011", "HOST 001", "", "HOST-001",
                                                   "LCMEE!", "H\xC3\xB6st", std::string("LCM\0E", 5)}) {
        CHECK_FALSE(IsValidCode(bad));
        CHECK(NormalizeCode(bad) == "a code is 1-8 letters and digits");
    }
}

TEST_CASE("an address prints as its dotted quad") {
    CHECK(Ipv4Text(kLoopback) == "127.0.0.1");
    CHECK(Ipv4Text(kLan) == "192.168.1.2");
    CHECK(Ipv4Text(0) == "0.0.0.0");
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
    REQUIRE(listen(s, 1) == 0);
    uint32_t address = 0;
    REQUIRE(band3::net::BoundAddress(s, port, address));
    port = ntohs(port);
    return s;
}

// exactly `size` bytes, or what came before the connection closed
Bytes ReadExactly(Handle s, size_t size) {
    Bytes bytes(size);
    size_t got = 0;
    while (got < size) {
        const int n = static_cast<int>(recv(s, reinterpret_cast<char*>(bytes.data() + got),
                                            static_cast<int>(size - got), 0));
        if (n <= 0) break;
        got += static_cast<size_t>(n);
    }
    bytes.resize(got);
    return bytes;
}

void Send(Handle s, const Bytes& bytes) {
    send(s, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()), 0);
}

// the server's side of a login, as the golden packets have it
void ServeLogin(Handle s) {
    ReadExactly(s, GoldenBytes("client_hello").size());
    Send(s, GoldenBytes("server_hello"));
    ReadExactly(s, GoldenBytes("client_login").size());
    Send(s, GoldenBytes("server_logged_in"));
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

// polls the client's status until `done` holds, for up to 5 s
template <typename F>
ClientStatus WaitFor(const Client& client, F done) {
    const auto deadline = Clock::now() + 5s;
    ClientStatus status = client.GetStatus();
    while (!done(status) && Clock::now() < deadline) {
        std::this_thread::sleep_for(10ms);
        status = client.GetStatus();
    }
    return status;
}

Config LoopbackConfig(uint16_t port) {
    Config config = GoldenConfig();
    config.server = "127.0.0.1:" + std::to_string(port);
    config.port = port;
    return config;
}

}  // namespace

TEST_CASE("the client logs in, joins and hears the server hang up, over TCP") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    Bytes hello, login, request;
    std::thread server([&] {
        const Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        hello = ReadExactly(s, GoldenBytes("client_hello").size());
        Send(s, GoldenBytes("server_hello"));
        login = ReadExactly(s, GoldenBytes("client_login").size());
        Send(s, GoldenBytes("server_logged_in"));
        request = ReadExactly(s, GoldenBytes("join_request").size());
        Send(s, GoldenBytes("join_denied"));
        // hang up once the client has the denial (or the test gives up)
        ReadExactly(s, 1);
        Close(s);
    });

    Client client;
    std::vector<uint32_t> joins;
    client.Start(LoopbackConfig(port), Callbacks{[&](uint32_t ip) {
                                                      joins.push_back(ip);
                                                      return std::string();
                                                  },
                                                  nullptr, nullptr});
    CHECK(client.Join("ABCD2345") == "not logged in to the Rooms server");
    ClientStatus status = WaitFor(client, [](const ClientStatus& s) { return s.state == State::kLoggedIn; });
    CHECK(status.state == State::kLoggedIn);
    CHECK(status.code == "ABCD2345");
    CHECK(client.PublicAddress() == kLoopback);
    CHECK(client.Join("abcd23456") == "a code is 1-8 letters and digits");
    CHECK(client.Join("abcd2345").empty());
    status = WaitFor(client, [](const ClientStatus& s) { return !s.error.empty(); });
    CHECK(status.error == "no game with code ABCD2345");
    CHECK(status.state == State::kLoggedIn);

    // the server waits for the client to close its end
    client.Stop();
    server.join();
    Close(listener);

    CHECK(Hex(hello) == Hex(GoldenBytes("client_hello")));
    CHECK(Hex(login) == Hex(GoldenBytes("client_login")));
    CHECK(Hex(request) == Hex(GoldenBytes("join_request")));
    CHECK(joins.empty());
}

TEST_CASE("the client says when the server hangs up after logging in") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    std::thread server([&] {
        const Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        ReadExactly(s, GoldenBytes("client_hello").size());
        Send(s, GoldenBytes("server_hello"));
        ReadExactly(s, GoldenBytes("client_login").size());
        Send(s, GoldenBytes("server_logged_in"));
        std::this_thread::sleep_for(300ms);
        Close(s);
    });
    Client client;
    client.Start(LoopbackConfig(port), {});
    const ClientStatus status =
        WaitFor(client, [](const ClientStatus& s) { return s.state == State::kDisconnected; });
    server.join();
    Close(listener);
    CHECK(status.state == State::kDisconnected);
    CHECK(status.error == "the server closed the connection");
    CHECK(status.code == "ABCD2345");
    // its address stays what the server saw
    CHECK(client.PublicAddress() == kLoopback);
}

TEST_CASE("the client says when nothing listens at the server's address") {
    uint16_t port = 0;
    Close(Listen(port));
    Client client;
    client.Start(LoopbackConfig(port), {});
    const ClientStatus status = WaitFor(client, [](const ClientStatus& s) { return s.state == State::kFailed; });
    CHECK(status.state == State::kFailed);
    CHECK(status.error == "127.0.0.1:" + std::to_string(port) + " refused the connection");
    CHECK(client.PublicAddress() == 0);
}

TEST_CASE("a client stops while it waits on its server, and starts over") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    // the server accepts (the backlog does) and says nothing
    Client client;
    client.Start(LoopbackConfig(port), {});
    ClientStatus status = WaitFor(client, [](const ClientStatus& s) { return s.state == State::kConnected; });
    CHECK(status.state == State::kConnected);
    const auto stopping = Clock::now();
    client.Stop();
    CHECK(Clock::now() - stopping < 2s);
    client.Start(LoopbackConfig(port), {});
    CHECK(client.GetStatus().state == State::kConnecting);
    client.Stop();
    Close(listener);
}

TEST_CASE("the client connects again by itself after the server drops it") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    std::thread server([&] {
        Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        ServeLogin(s);
        std::this_thread::sleep_for(300ms);
        Close(s);
        s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        ServeLogin(s);
        // until the client closes its end
        ReadExactly(s, 1);
        Close(s);
    });
    Client client;
    Config config = LoopbackConfig(port);
    config.retry_delays = {1200ms};
    client.Start(config, {});
    ClientStatus status = WaitFor(client, [](const ClientStatus& s) { return s.state == State::kLoggedIn; });
    CHECK(status.state == State::kLoggedIn);
    CHECK(status.attempt == 1);
    CHECK(status.retry_in_s == 0);
    // the wait says how long it has left, rounded up, while the state stays
    status = WaitFor(client, [](const ClientStatus& s) { return s.retry_in_s > 0; });
    CHECK(status.state == State::kDisconnected);
    CHECK(status.error == "the server closed the connection");
    CHECK(status.retry_in_s >= 1);
    CHECK(status.retry_in_s <= 2);
    CHECK(status.attempt == 1);
    status = WaitFor(client, [](const ClientStatus& s) { return s.state == State::kLoggedIn; });
    CHECK(status.state == State::kLoggedIn);
    CHECK(status.attempt == 2);
    CHECK(status.retry_in_s == 0);
    CHECK(status.code == "ABCD2345");
    CHECK(status.error.empty());
    client.Stop();
    server.join();
    Close(listener);
}

TEST_CASE("the client doesn't try again once the server refused its login") {
    uint16_t port = 0;
    const Handle listener = Listen(port);
    bool again = true;
    std::thread server([&] {
        const Handle s = accept(listener, nullptr, nullptr);
        if (s == kNone) return;
        ReadExactly(s, GoldenBytes("client_hello").size());
        Send(s, GoldenBytes("server_hello"));
        ReadExactly(s, GoldenBytes("client_login").size());
        // as a Rooms server says no to a login: it hangs up
        Close(s);
        again = Incoming(listener, 1000ms);
    });
    Client client;
    Config config = LoopbackConfig(port);
    config.retry_delays = {100ms};
    client.Start(config, {});
    server.join();
    Close(listener);
    CHECK_FALSE(again);
    const ClientStatus status = client.GetStatus();
    CHECK(status.state == State::kFailed);
    CHECK(status.error.starts_with("login refused"));
    CHECK(status.retry_in_s == 0);
    CHECK(status.attempt == 1);
}

TEST_CASE("Stop ends the wait to connect again at once") {
    uint16_t port = 0;
    Close(Listen(port));
    Client client;
    // the first wait, 5 s
    client.Start(LoopbackConfig(port), {});
    ClientStatus status = WaitFor(client, [](const ClientStatus& s) { return s.retry_in_s > 0; });
    CHECK(status.state == State::kFailed);
    CHECK(status.error == "127.0.0.1:" + std::to_string(port) + " refused the connection");
    CHECK(status.retry_in_s == 5);
    CHECK(status.attempt == 1);
    const auto stopping = Clock::now();
    client.Stop();
    CHECK(Clock::now() - stopping < 2s);
    status = client.GetStatus();
    CHECK(status.state == State::kFailed);
    CHECK(status.retry_in_s == 0);
}
