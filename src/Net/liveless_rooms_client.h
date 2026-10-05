#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "liveless_rooms.h"
#include "liveless_rooms_protocol.h"

// The connection to a Liveless Rooms server: log in for a code, answer its
// pings, ask it for the game a code belongs to, and hand on what it says. The
// game's side of it (the invite a join becomes, the NAT punch from the game's
// socket) is the caller's, through Callbacks; src/Hooks/liveless_rooms.cpp
// wires it to the game. Plain sockets, no SDK, so it builds and is tested off
// Windows too.
namespace band3::rooms {

// a network-order IPv4 address as a.b.c.d
inline std::string Ipv4Text(uint32_t address) {
    return std::to_string(address & 0xFF) + "." + std::to_string((address >> 8) & 0xFF) + "." +
           std::to_string((address >> 16) & 0xFF) + "." + std::to_string(address >> 24);
}

// A join code as the server keeps them, in upper case. Returns why `code`
// isn't one (1-8 letters and digits), or empty.
std::string NormalizeCode(std::string& code);

// Who logs in, and where to.
struct Config {
    std::string server;  // liveless_rooms_server as typed, host[:port], for messages
    // its host part, as typed: what's looked up, and what the login proof
    // signs, which the server checks against its own address
    std::string host;
    uint16_t port = kPort;
    std::string gamertag;  // up to 15 characters
    uint64_t xuid = 0;
    uint32_t local_ipv4 = 0;  // this PC's on its own network, for players behind the same router
    std::string version;
    std::string language = "eng";
    // How long Client waits before connecting again after a connection is
    // lost, one after another, the last one from then on; logging in starts
    // them over. Long enough apart that a server that's down isn't hammered by
    // every game waiting on it. Empty: never by itself.
    std::vector<std::chrono::milliseconds> retry_delays{std::chrono::seconds(5), std::chrono::seconds(10),
                                                        std::chrono::seconds(30), std::chrono::seconds(60)};
};

// The connection as far as it got. IPv4 addresses in network order.
struct ClientStatus {
    State state = State::kOff;
    std::string code;
    uint32_t public_ipv4 = 0;
    std::string error;
    std::string last_join_user;
    uint32_t last_join_ipv4 = 0;
    // seconds until Client connects again by itself, rounded up; 0 when it
    // isn't going to
    int retry_in_s = 0;
    // which connection this is since Start, from 1
    int attempt = 0;
};

// What the server asks of the game, on the connection's thread.
struct Callbacks {
    // join the game at this address (the host's, port 9103); an error, or empty
    std::function<std::string(uint32_t ipv4)> on_join;
    // a player at this address is joining this game: open the way to them
    std::function<void(uint32_t ipv4)> on_nat_punch;
    // a line for the log
    std::function<void(const std::string& line)> log;
};

// The protocol's side of a connection, without the socket: it's told what
// happened to the connection and what came in, and answers with the bytes to
// send. Not thread-safe; Client's thread owns one per connection.
class Session {
public:
    using Clock = std::chrono::steady_clock;
    // RB3Enhanced's: the server pings well within it
    static constexpr std::chrono::seconds kSilence{30};

    Session(Config config, Callbacks callbacks);

    // the connection is up: what to send first (ClientHello)
    Bytes Connected(Clock::time_point now);
    // bytes from the server, in whatever pieces they came: what to send back
    Bytes Received(const uint8_t* data, size_t size, Clock::time_point now);
    // asks for the game with `code` (NormalizeCode's): the request, or nothing
    // when not logged in
    Bytes Join(const std::string& code);
    // the server closed the connection, or it broke
    void Closed();
    // the connection was never made: looking up or connecting failed
    void Failed(std::string error);
    // time passing without a word from the server
    void Tick(Clock::time_point now);
    // whether the connection is done with, failed or disconnected, and should close
    bool Over() const { return status_.state == State::kFailed || status_.state == State::kDisconnected; }
    // Whether it's worth connecting again, once Over: a connection that was
    // logged in and went, or one that never reached a server. Not once the
    // server has had its say before logging in (turned the connection away,
    // refused the login, sent what isn't Rooms): asking again gets the same.
    bool Retry() const {
        return status_.state == State::kDisconnected || (status_.state == State::kFailed && !server_spoke_);
    }

    const ClientStatus& status() const { return status_; }

private:
    void Handle(const Frame& frame, Bytes& out);
    // the connection is lost: failed before logging in, disconnected after
    void Lose(const std::string& error);
    void Log(const std::string& line) const;

    Config config_;
    Callbacks callbacks_;
    ClientStatus status_;
    FrameReader reader_;
    Clock::time_point last_heard_{};
    // the code of the last join asked for, which a denial is about
    std::string joining_;
    // the server sent something: its hello, or bytes that aren't Rooms
    bool server_spoke_ = false;
};

// A Session on a thread of its own, over TCP. Start connects and logs in, and
// when the connection is lost (Session::Retry) the thread connects again by
// itself after Config::retry_delays, a new Session each time; Start again
// connects at once. Everything here is safe from any thread; GetStatus and
// PublicAddress never wait on the connection.
class Client {
public:
    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() { Stop(); }

    // ends any connection there is, then starts a new one
    void Start(Config config, Callbacks callbacks);
    // ends the connection, or the wait to connect again, and waits for its
    // thread; the status stays as it was, but for retry_in_s, now 0
    void Stop();
    // asks for the game with `code`: an error, or empty once it's on its way
    std::string Join(std::string code);
    ClientStatus GetStatus() const;
    // the server's view of this PC's address, 0 until logged in; kept through
    // the thread's own reconnects and Stop, 0 again at Start; never locks
    uint32_t PublicAddress() const { return public_ipv4_; }

private:
    void Run(Config config, Callbacks callbacks);
    // one connection, from looking the server up until it's over or stopped
    void Attempt(Session& session, const Config& config, const Callbacks& callbacks);
    // waits out `delay` with the status saying so; false if stopped
    bool Backoff(const Session& session, std::chrono::milliseconds delay);
    void Publish(const Session& session, int retry_in_s = 0);
    // the joins asked for since the thread last looked
    std::vector<std::string> TakeJoins();

    // Start and Stop, one at a time
    std::mutex control_mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    // the thread's: which connection it's on, for the status
    int attempt_ = 0;
    mutable std::mutex mutex_;
    ClientStatus status_;
    std::vector<std::string> joins_;
    std::atomic<uint32_t> public_ipv4_{0};
};

}  // namespace band3::rooms
