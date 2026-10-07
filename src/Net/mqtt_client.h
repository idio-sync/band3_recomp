#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include "mqtt_protocol.h"

// A connection to an MQTT broker that only publishes: connect, keep it alive,
// publish what the caller produces, and connect again when it drops. Plain
// sockets, no SDK, so it builds and is tested off Windows too.
namespace band3::mqtt {

// The protocol's side of one connection, without the socket: it's told what
// happened to the connection and what came in, and answers with the bytes to
// send. Not thread-safe; Client's thread owns one per connection.
class Session {
public:
    using Clock = std::chrono::steady_clock;
    // how long the broker may take to answer CONNECT, and a PINGREQ
    static constexpr std::chrono::seconds kConnackWait{5};
    static constexpr std::chrono::seconds kPingWait{30};
    enum class State { kConnecting, kConnected, kRefused, kLost };

    explicit Session(ConnectOptions options);

    // the connection is up: the CONNECT to send, and the wait for CONNACK starts
    Bytes Connected(Clock::time_point now);
    // bytes from the broker, in whatever pieces they came
    void Received(const uint8_t* data, size_t size, Clock::time_point now);
    // time passing: a PINGREQ every keepalive/2 while connected, and the
    // connection lost when CONNACK or PINGRESP is overdue
    Bytes Tick(Clock::time_point now);
    // the PUBLISH for `message`; empty unless connected
    Bytes Publish(const Message& message);
    // the connection dropped
    void Closed();
    // the connection was never made: looking up or connecting failed
    void Failed(std::string error);

    State state() const { return state_; }
    // why it's lost or refused
    const std::string& error() const { return error_; }
    bool Over() const { return state_ == State::kRefused || state_ == State::kLost; }
    // Whether connecting again could help, once Over: a lost connection, or a
    // refusal the broker may get over (1-3). A bad user name or password, or
    // not being authorized, takes a settings change.
    bool Retry() const;
    // the broker accepted this connection at some point
    bool WasConnected() const { return was_connected_; }

private:
    void Handle(const Packet& packet, Clock::time_point now);
    void Lose(std::string error);

    ConnectOptions options_;
    State state_ = State::kConnecting;
    std::string error_;
    PacketReader reader_;
    int refused_code_ = 0;
    bool was_connected_ = false;
    // when CONNECT went, for its CONNACK's wait
    std::optional<Clock::time_point> connect_sent_;
    // when the last PINGREQ went (or the connection was accepted)
    Clock::time_point last_ping_{};
    // a PINGREQ with no PINGRESP yet
    std::optional<Clock::time_point> ping_outstanding_;
};

struct ClientStatus {
    enum class State { kOff, kConnecting, kConnected, kRetrying, kRefused };
    State state = State::kOff;
    std::string error;
    // seconds until the next connection, rounded up, while kRetrying
    int retry_in_s = 0;
};

// "off", "connecting", "connected", "retrying in N s", "refused: <error>"
std::string StatusText(const ClientStatus& status);

struct ClientConfig {
    std::string host;
    uint16_t port = 1883;
    ConnectOptions connect;
    // waits before each reconnect, one after another, the last repeating; a
    // connection that stayed connected for `stable_after` starts them over
    std::vector<std::chrono::milliseconds> retry_delays{
        std::chrono::milliseconds(0), std::chrono::seconds(5), std::chrono::seconds(10),
        std::chrono::seconds(20), std::chrono::seconds(40), std::chrono::seconds(60)};
    // How long a connection must last to count as the broker being fine. One
    // that accepts and then drops band3 at once (another client taking over
    // its id, an ACL) goes on stepping through the waits, not reconnecting at
    // once forever.
    std::chrono::milliseconds stable_after = std::chrono::seconds(60);
    // lines without a prefix; a failure is logged only when its text differs
    // from the last one logged, unless a lasting connection came between
    std::function<void(const std::string&)> log;
    // how the host is looked up, on a thread of its own; net::ResolveIPv4
    // when empty (tests slow it down)
    std::function<std::vector<uint32_t>(const std::string& host)> resolve;
};

// A Session on a thread of its own, over TCP, like rooms::Client. While
// connected it calls `produce` about every 50 ms on its thread and publishes
// what it returns; `fresh` is true on the first call after each (re)connect
// (the caller resends everything then). Start, Stop and GetStatus are safe
// from any thread.
class Client {
public:
    using Produce = std::function<std::vector<Message>(bool fresh)>;

    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    ~Client() { Stop(); }

    // ends any connection there is, then starts a new one
    void Start(ClientConfig config, Produce produce);
    // publishes `last` (e.g. availability "offline") and DISCONNECT if
    // connected, closes, joins the thread; the status is kOff after it, but
    // for a refusal, which stays to say what to change
    void Stop(std::vector<Message> last = {});
    ClientStatus GetStatus() const;

private:
    void Run(const ClientConfig& config, const Produce& produce);
    // one connection, from looking the broker up until it's over or stopped;
    // true if it stayed connected for config.stable_after
    bool Attempt(Session& session, const ClientConfig& config, const Produce& produce);
    // the broker's addresses, waited for in kWake slices; empty if stopped first
    std::vector<uint32_t> Resolve(const ClientConfig& config);
    // waits out `delay` with the status saying so; false if stopped
    bool Backoff(std::chrono::milliseconds delay, const std::string& error);
    void SetStatus(ClientStatus::State state, std::string error = {}, int retry_in_s = 0);
    void Log(const ClientConfig& config, const std::string& line) const;
    // a failure, unless it's the one logged last
    void LogFailure(const ClientConfig& config, const std::string& error);

    // Start and Stop, one at a time
    std::mutex control_mutex_;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    mutable std::mutex mutex_;
    ClientStatus status_;
    // what Stop publishes before it disconnects
    std::vector<Message> last_;
    // the thread's: the failure logged last, empty after a lasting connection
    std::string last_failure_;
    // the thread's: "connected to" was logged since the last failure was, so
    // a broker that keeps dropping band3 doesn't log it on every flap
    bool connected_logged_ = false;
};

}  // namespace band3::mqtt
