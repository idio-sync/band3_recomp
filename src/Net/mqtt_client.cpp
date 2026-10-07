#include "mqtt_client.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <utility>
#include "native_socket.h"

namespace band3::mqtt {

namespace {

using namespace std::chrono_literals;

// how long a broker may take to take the connection
constexpr auto kConnectTimeout = 5s;
// how long the thread waits on the socket before it publishes what's new and
// looks at whether to stop
constexpr auto kWake = 50ms;

// The socket helpers are liveless_rooms_client.cpp's, kept apart so neither
// connection's needs bend the other's.
#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) {
    // what was sent (a DISCONNECT) still goes before the close
    shutdown(s, SD_SEND);
    closesocket(s);
}
int LastError() { return WSAGetLastError(); }
bool InProgress(int error) { return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS; }
bool Refused(int error) { return error == WSAECONNREFUSED; }
bool SetNonBlocking(socket_t s) {
    u_long on = 1;
    return ioctlsocket(s, FIONBIO, &on) == 0;
}
constexpr int kSendFlags = 0;
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) {
    shutdown(s, SHUT_WR);
    close(s);
}
int LastError() { return errno; }
bool InProgress(int error) { return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN; }
bool Refused(int error) { return error == ECONNREFUSED; }
bool SetNonBlocking(socket_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
// a broker that went away is an error to send to, not a SIGPIPE
#ifdef MSG_NOSIGNAL
constexpr int kSendFlags = MSG_NOSIGNAL;
#else
constexpr int kSendFlags = 0;
#endif
#endif

// what select found on one socket within `wait`
enum class Ready { kNone, kYes, kError };
Ready Wait(socket_t s, bool write, std::chrono::milliseconds wait) {
    fd_set set, errors;
    FD_ZERO(&set);
    FD_ZERO(&errors);
    FD_SET(s, &set);
    FD_SET(s, &errors);
    timeval timeout{static_cast<long>(wait.count() / 1000), static_cast<long>((wait.count() % 1000) * 1000)};
#ifdef _WIN32
    const int nfds = 0;  // ignored on Windows
#else
    const int nfds = s + 1;
#endif
    // Windows reports a failed connect in the error set, not as writable
    const int n = select(nfds, write ? nullptr : &set, write ? &set : nullptr, &errors, &timeout);
    if (n < 0) return Ready::kError;
    return n == 0 ? Ready::kNone : Ready::kYes;
}

// all of `bytes`, waiting a little for room if the socket has none; false if
// the connection is gone
bool SendAll(socket_t s, const Bytes& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const int n = static_cast<int>(send(s, reinterpret_cast<const char*>(bytes.data() + sent),
                                            static_cast<int>(bytes.size() - sent), kSendFlags));
        if (n > 0) {
            sent += static_cast<size_t>(n);
        } else if (n < 0 && InProgress(LastError())) {
            if (Wait(s, true, 1000ms) != Ready::kYes) return false;
        } else {
            return false;
        }
    }
    return true;
}

}  // namespace

Session::Session(ConnectOptions options) : options_(std::move(options)) {}

Bytes Session::Connected(Clock::time_point now) {
    state_ = State::kConnecting;
    connect_sent_ = now;
    return EncodeConnect(options_);
}

void Session::Received(const uint8_t* data, size_t size, Clock::time_point now) {
    if (Over()) return;
    reader_.Add(data, size);
    while (!Over()) {
        auto packet = reader_.Next();
        if (!packet) break;
        Handle(*packet, now);
    }
    if (!Over() && reader_.Bad()) Lose("bad data from the broker");
}

void Session::Handle(const Packet& packet, Clock::time_point now) {
    if (packet.type == kConnack) {
        // a second one changes nothing
        if (state_ != State::kConnecting) return;
        const int code = ConnackCode(packet);
        if (code < 0) {
            Lose("bad data from the broker");
        } else if (code == 0) {
            state_ = State::kConnected;
            was_connected_ = true;
            error_.clear();
            last_ping_ = now;
        } else {
            state_ = State::kRefused;
            refused_code_ = code;
            error_ = ConnackText(code);
        }
    } else if (packet.type == kPingResp) {
        ping_outstanding_.reset();
    }
    // nothing else is asked for (no subscriptions, QoS 0 only); the reader
    // has already stepped over it by its length
}

Bytes Session::Tick(Clock::time_point now) {
    if (state_ == State::kConnecting) {
        if (connect_sent_ && now - *connect_sent_ > kConnackWait) Lose("no answer from the broker");
        return {};
    }
    if (state_ != State::kConnected) return {};
    if (ping_outstanding_) {
        if (now - *ping_outstanding_ > kPingWait) Lose("the broker stopped answering");
        return {};
    }
    // half the keepalive, so the broker hears from us well within it
    const auto interval = std::chrono::milliseconds(options_.keepalive_s) * 1000 / 2;
    if (options_.keepalive_s == 0 || now - last_ping_ < interval) return {};
    last_ping_ = now;
    ping_outstanding_ = now;
    return EncodePingReq();
}

Bytes Session::Publish(const Message& message) {
    if (state_ != State::kConnected) return {};
    return EncodePublish(message);
}

void Session::Closed() {
    if (Over()) return;
    Lose("the broker closed the connection");
}

void Session::Failed(std::string error) { Lose(std::move(error)); }

bool Session::Retry() const {
    return state_ == State::kLost || (state_ == State::kRefused && refused_code_ >= 1 && refused_code_ <= 3);
}

void Session::Lose(std::string error) {
    state_ = State::kLost;
    error_ = std::move(error);
}

std::string StatusText(const ClientStatus& status) {
    switch (status.state) {
    case ClientStatus::State::kOff: return "off";
    case ClientStatus::State::kConnecting: return "connecting";
    case ClientStatus::State::kConnected: return "connected";
    case ClientStatus::State::kRetrying: return "retrying in " + std::to_string(status.retry_in_s) + " s";
    case ClientStatus::State::kRefused: return "refused: " + status.error;
    }
    return "off";
}

void Client::Start(ClientConfig config, Produce produce) {
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.state = ClientStatus::State::kConnecting;
        last_.clear();
    }
    // the thread isn't running yet, and starting it publishes this to it
    last_failure_.clear();
    thread_ = std::thread([this, config = std::move(config), produce = std::move(produce)] {
        Run(config, produce);
    });
}

void Client::Stop(std::vector<Message> last) {
    std::lock_guard<std::mutex> control(control_mutex_);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        last_ = std::move(last);
    }
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    last_.clear();
    // a refusal stays to be read: it says what to change
    if (status_.state == ClientStatus::State::kRefused) {
        status_.retry_in_s = 0;
    } else {
        status_ = {};
    }
}

ClientStatus Client::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Client::SetStatus(ClientStatus::State state, std::string error, int retry_in_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_.state = state;
    status_.error = std::move(error);
    status_.retry_in_s = retry_in_s;
}

void Client::Log(const ClientConfig& config, const std::string& line) const {
    if (config.log) config.log(line);
}

void Client::LogFailure(const ClientConfig& config, const std::string& error) {
    if (error == last_failure_) return;
    last_failure_ = error;
    Log(config, error);
}

// Stop waits for this: every wait in it is cut into kWake slices that check
// stop_, except the name lookup, which is the system's
void Client::Run(const ClientConfig& config, const Produce& produce) {
#ifdef _WIN32
    // Winsock counts startups, so this one is harmless beside the SDK's
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) {
        // nothing a retry would change
        LogFailure(config, "Winsock didn't start");
        SetStatus(ClientStatus::State::kRefused, "Winsock didn't start");
        return;
    }
#endif
    // which of retry_delays the next wait is
    size_t backoff = 0;
    while (!stop_) {
        Session session(config.connect);
        SetStatus(ClientStatus::State::kConnecting);
        Attempt(session, config, produce);
        if (stop_ || !session.Over()) return;
        LogFailure(config, session.error());
        if (!session.Retry()) {
            SetStatus(ClientStatus::State::kRefused, session.error());
            return;
        }
        // the broker was fine until now: the waits start over
        if (session.WasConnected()) backoff = 0;
        std::chrono::milliseconds delay{0};
        if (!config.retry_delays.empty()) {
            delay = config.retry_delays[std::min(backoff, config.retry_delays.size() - 1)];
        }
        backoff++;
        if (delay > std::chrono::milliseconds(0) && !Backoff(delay, session.error())) return;
    }
}

bool Client::Backoff(std::chrono::milliseconds delay, const std::string& error) {
    const auto until = Session::Clock::now() + delay;
    while (!stop_) {
        const auto left = until - Session::Clock::now();
        if (left <= Session::Clock::duration::zero()) return true;
        // rounded up, so it says 1 until the moment it connects, never 0
        const int seconds = static_cast<int>(std::chrono::ceil<std::chrono::seconds>(left).count());
        SetStatus(ClientStatus::State::kRetrying, error, std::max(1, seconds));
        std::this_thread::sleep_for(std::min<Session::Clock::duration>(kWake, left));
    }
    return false;
}

void Client::Attempt(Session& session, const ClientConfig& config, const Produce& produce) {
    const std::string where = config.host + ":" + std::to_string(config.port);
    // once, not on every retry of a broker that's down
    if (last_failure_.empty()) Log(config, "connecting to " + where);
    const std::vector<uint32_t> found = net::ResolveIPv4(config.host);
    if (found.empty()) {
        session.Failed("no IPv4 address for " + config.host);
        return;
    }
    const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket || !SetNonBlocking(s)) {
        if (s != kNoSocket) CloseSocket(s);
        session.Failed("couldn't make a socket for " + where);
        return;
    }
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(config.port);
    address.sin_addr.s_addr = found.front();

    // connecting: non-blocking, so it can time out and be stopped
    bool connected = false;
    std::string failure;
    const int result = connect(s, reinterpret_cast<const sockaddr*>(&address), sizeof(address));
    const int error = result == 0 ? 0 : LastError();
    if (result == 0) {
        connected = true;
    } else if (!InProgress(error)) {
        failure = Refused(error) ? where + " refused the connection" : "couldn't connect to " + where;
    } else {
        const auto deadline = Session::Clock::now() + kConnectTimeout;
        while (!stop_) {
            const Ready ready = Wait(s, true, kWake);
            if (ready == Ready::kError) {
                failure = "couldn't connect to " + where;
                break;
            }
            if (ready == Ready::kYes) {
                int outcome = 0;
#ifdef _WIN32
                int length = sizeof(outcome);
#else
                socklen_t length = sizeof(outcome);
#endif
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&outcome), &length);
                if (outcome == 0) {
                    connected = true;
                } else {
                    failure = Refused(outcome) ? where + " refused the connection" : "no answer from " + where;
                }
                break;
            }
            if (Session::Clock::now() >= deadline) {
                failure = "no answer from " + where;
                break;
            }
        }
    }
    if (!connected) {
        CloseSocket(s);
        if (!stop_) session.Failed(failure);
        return;
    }

    if (!SendAll(s, session.Connected(Session::Clock::now()))) session.Closed();
    // the first produce after CONNACK resends everything
    bool fresh = true;
    uint8_t buffer[1024];
    while (!stop_ && !session.Over()) {
        const Ready ready = Wait(s, false, kWake);
        const auto now = Session::Clock::now();
        if (ready == Ready::kError) {
            session.Closed();
        } else if (ready == Ready::kYes) {
            const int n = static_cast<int>(recv(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
            if (n > 0) {
                session.Received(buffer, static_cast<size_t>(n), now);
            } else if (n == 0 || !InProgress(LastError())) {
                session.Closed();
            }
        }
        if (!session.Over()) {
            const Bytes ping = session.Tick(now);
            if (!ping.empty() && !SendAll(s, ping)) session.Closed();
        }
        if (session.state() != Session::State::kConnected) continue;
        if (fresh) {
            Log(config, "connected to " + where);
            // so the next failure is logged, even one like the last
            last_failure_.clear();
            SetStatus(ClientStatus::State::kConnected);
        }
        if (produce) {
            for (const Message& message : produce(fresh)) {
                if (session.Over()) break;
                if (!SendAll(s, session.Publish(message))) session.Closed();
            }
        }
        fresh = false;
    }
    if (stop_ && session.state() == Session::State::kConnected) {
        std::vector<Message> last;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            last = std::move(last_);
        }
        bool open = true;
        for (const Message& message : last) {
            if (open) open = SendAll(s, session.Publish(message));
        }
        if (open) SendAll(s, EncodeDisconnect());
    }
    CloseSocket(s);
}

}  // namespace band3::mqtt
