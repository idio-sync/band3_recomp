#include "liveless_rooms_client.h"

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
#include <cstdio>
#include <utility>
#include <variant>
#include "native_socket.h"

namespace band3::rooms {

namespace {

using namespace std::chrono_literals;

// how long a connection may take to answer, as RB3Enhanced's client waits
constexpr auto kConnectTimeout = 4s;
// how long the thread waits on the socket before it looks at its joins and
// whether to stop
constexpr auto kWake = 250ms;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
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
void CloseSocket(socket_t s) { close(s); }
int LastError() { return errno; }
bool InProgress(int error) { return error == EINPROGRESS || error == EWOULDBLOCK || error == EAGAIN; }
bool Refused(int error) { return error == ECONNREFUSED; }
bool SetNonBlocking(socket_t s) {
    const int flags = fcntl(s, F_GETFL, 0);
    return flags >= 0 && fcntl(s, F_SETFL, flags | O_NONBLOCK) == 0;
}
// a server that went away is an error to send to, not a SIGPIPE
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

std::string NormalizeCode(std::string& code) {
    bool ok = code.size() == 8;
    for (char& c : code) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        ok &= (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    }
    return ok ? std::string() : "a code is 8 letters and digits";
}

Session::Session(Config config, Callbacks callbacks)
    : config_(std::move(config)), callbacks_(std::move(callbacks)) {}

Bytes Session::Connected(Clock::time_point now) {
    status_.state = State::kConnected;
    last_heard_ = now;
    Log("rooms: connected to " + config_.server + ", logging in as " + config_.gamertag);
    ClientHello hello;
    hello.emulator = true;
    hello.language = config_.language;
    hello.version = config_.version;
    return Encode(hello);
}

Bytes Session::Received(const uint8_t* data, size_t size, Clock::time_point now) {
    Bytes out;
    if (Over()) return out;
    last_heard_ = now;
    if (size > 0) server_spoke_ = true;
    reader_.Feed(data, size);
    while (!Over()) {
        auto frame = reader_.Next();
        if (!frame) break;
        Handle(*frame, out);
    }
    if (!Over() && reader_.Broken()) Lose("bad data from the server");
    return out;
}

void Session::Handle(const Frame& frame, Bytes& out) {
    const auto message = DecodeServer(frame);
    if (!message) {
        Log("rooms: a packet of type " + std::to_string(frame.type) + " too short to read, ignored");
        return;
    }
    if (const auto* hello = std::get_if<ServerHello>(&*message)) {
        if (status_.state != State::kConnected) {
            Log("rooms: a second hello from the server, ignored");
            return;
        }
        if (!hello->allowed) {
            status_.state = State::kFailed;
            status_.error = "the server turned the connection away";
            Log("rooms: " + status_.error);
            return;
        }
        ClientLogin login;
        login.xuid = config_.xuid;
        login.gamertag = config_.gamertag;
        login.local_ipv4 = config_.local_ipv4;
        // the address as typed: the server signs the one it was told it's at
        if (hello->needs_proof) login.proof = LoginProof(hello->proof_key, config_.host, config_.xuid);
        const Bytes packet = Encode(login);
        out.insert(out.end(), packet.begin(), packet.end());
    } else if (const auto* logged_in = std::get_if<ServerLoggedIn>(&*message)) {
        status_.state = State::kLoggedIn;
        status_.code = logged_in->code;
        status_.public_ipv4 = logged_in->public_ipv4;
        status_.error.clear();
        Log("rooms: logged in as " + status_.code + ", public IP " + Ipv4Text(status_.public_ipv4));
    } else if (std::holds_alternative<Ping>(*message)) {
        const Bytes pong = Encode(Pong{});
        out.insert(out.end(), pong.begin(), pong.end());
    } else if (const auto* response = std::get_if<JoinResponse>(&*message)) {
        // behind the same router as the host, the server sees both at one
        // address, which only reaches the host from outside: go to its own
        const bool same = response->public_ipv4 == status_.public_ipv4;
        const uint32_t first = same ? response->private_ipv4 : response->public_ipv4;
        const uint32_t second = same ? response->public_ipv4 : response->private_ipv4;
        const uint32_t address = first ? first : second;
        if (!address) {
            status_.error = "the server gave no address for " + response->user;
            Log("rooms: " + status_.error);
            return;
        }
        status_.last_join_user = response->user;
        status_.last_join_ipv4 = address;
        Log("rooms: joining " + response->user + " at " + Ipv4Text(address));
        if (callbacks_.on_join) {
            std::string error = callbacks_.on_join(address);
            if (!error.empty()) {
                status_.error = std::move(error);
                Log("rooms: the join failed: " + status_.error);
            }
        }
    } else if (const auto* denied = std::get_if<JoinDenied>(&*message)) {
        status_.error = denied->reason == 0 ? "no game with code " + joining_
                                            : "join denied (reason " + std::to_string(denied->reason) + ")";
        Log("rooms: " + status_.error);
    } else if (const auto* punch = std::get_if<NatPunchRequest>(&*message)) {
        if (callbacks_.on_nat_punch) callbacks_.on_nat_punch(punch->public_ipv4);
    } else if (const auto* unknown = std::get_if<Unknown>(&*message)) {
        Log("rooms: a packet of unknown type " + std::to_string(unknown->type) + ", ignored");
    }
}

Bytes Session::Join(const std::string& code) {
    if (status_.state != State::kLoggedIn) return {};
    joining_ = code;
    status_.error.clear();
    Log("rooms: asking for the game with code " + code);
    return Encode(JoinRequest{code});
}

void Session::Closed() {
    if (Over()) return;
    if (status_.state == State::kLoggedIn) {
        Lose("the server closed the connection");
        return;
    }
    // the server says nothing when it turns a login away, it hangs up; most
    // often the proof didn't match the address it was signed with
    status_.state = State::kFailed;
    status_.error = "login refused: check that liveless_rooms_server matches the server's address, "
                    "and username";
    Log("rooms: " + status_.error);
}

void Session::Failed(std::string error) {
    status_.state = State::kFailed;
    status_.error = std::move(error);
    Log("rooms: " + status_.error);
}

void Session::Tick(Clock::time_point now) {
    const bool connected = status_.state == State::kConnected || status_.state == State::kLoggedIn;
    if (connected && now - last_heard_ > kSilence) Lose("the server stopped answering");
}

void Session::Lose(const std::string& error) {
    status_.state = status_.state == State::kLoggedIn ? State::kDisconnected : State::kFailed;
    status_.error = error;
    Log("rooms: " + error);
}

void Session::Log(const std::string& line) const {
    if (callbacks_.log) callbacks_.log(line);
}

void Client::Start(Config config, Callbacks callbacks) {
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    stop_ = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        status_ = {};
        status_.state = State::kConnecting;
        status_.attempt = 1;
        joins_.clear();
    }
    public_ipv4_ = 0;
    // the thread isn't running yet, and starting it publishes this to it
    attempt_ = 0;
    thread_ = std::thread([this, config = std::move(config), callbacks = std::move(callbacks)] {
        Run(config, callbacks);
    });
}

void Client::Stop() {
    std::lock_guard<std::mutex> control(control_mutex_);
    stop_ = true;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    status_.retry_in_s = 0;
}

std::string Client::Join(std::string code) {
    if (std::string error = NormalizeCode(code); !error.empty()) return error;
    std::lock_guard<std::mutex> lock(mutex_);
    if (status_.state != State::kLoggedIn) return "not logged in to the Rooms server";
    joins_.push_back(std::move(code));
    return {};
}

ClientStatus Client::GetStatus() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return status_;
}

void Client::Publish(const Session& session, int retry_in_s) {
    std::lock_guard<std::mutex> lock(mutex_);
    status_ = session.status();
    status_.attempt = attempt_;
    status_.retry_in_s = retry_in_s;
    if (status_.public_ipv4) public_ipv4_ = status_.public_ipv4;
}

std::vector<std::string> Client::TakeJoins() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::exchange(joins_, {});
}

// Stop waits for this: every wait in it is cut into kWake slices that check
// stop_, except the name lookup, which is the system's
void Client::Run(Config config, Callbacks callbacks) {
#ifdef _WIN32
    // Winsock counts startups, so this one is harmless beside the SDK's
    static const bool started = [] {
        WSADATA data;
        return WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }();
    if (!started) {
        // nothing a retry would change
        Session session(config, callbacks);
        attempt_ = 1;
        session.Failed("Winsock didn't start");
        Publish(session);
        return;
    }
#endif
    // which of retry_delays the next wait is
    size_t backoff = 0;
    while (true) {
        attempt_++;
        Session session(config, callbacks);
        if (attempt_ > 1) {
            // what the last connection had, its code and joins, went with it
            std::lock_guard<std::mutex> lock(mutex_);
            status_ = {};
            status_.state = State::kConnecting;
            status_.attempt = attempt_;
            joins_.clear();
        }
        Attempt(session, config, callbacks);
        if (stop_ || !session.Retry() || config.retry_delays.empty()) return;
        // it logged in, so the server was fine until now: the waits start over
        if (session.status().state == State::kDisconnected) backoff = 0;
        const auto delay = config.retry_delays[std::min(backoff, config.retry_delays.size() - 1)];
        backoff++;
        if (callbacks.log) {
            const auto seconds = std::chrono::duration<double>(delay).count();
            char text[32];
            std::snprintf(text, sizeof(text), "%g", seconds);
            callbacks.log("rooms: connecting again in " + std::string(text) + " s (attempt " +
                          std::to_string(attempt_ + 1) + ")");
        }
        if (!Backoff(session, delay)) return;
    }
}

bool Client::Backoff(const Session& session, std::chrono::milliseconds delay) {
    const auto until = Session::Clock::now() + delay;
    while (!stop_) {
        const auto left = until - Session::Clock::now();
        if (left <= Session::Clock::duration::zero()) return true;
        // rounded up, so it says 1 until the moment it connects, never 0
        const int seconds = static_cast<int>(std::chrono::ceil<std::chrono::seconds>(left).count());
        Publish(session, std::max(1, seconds));
        std::this_thread::sleep_for(std::min<Session::Clock::duration>(kWake, left));
    }
    return false;
}

void Client::Attempt(Session& session, const Config& config, const Callbacks& callbacks) {
    const std::vector<uint32_t> found = net::ResolveIPv4(config.host);
    if (found.empty()) {
        session.Failed("no IPv4 address for " + config.server);
        Publish(session);
        return;
    }
    if (callbacks.log) {
        callbacks.log("rooms: connecting to " + config.server + " (" + Ipv4Text(found.front()) + ":" +
                      std::to_string(config.port) + ")");
    }
    const socket_t s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == kNoSocket || !SetNonBlocking(s)) {
        if (s != kNoSocket) CloseSocket(s);
        session.Failed("couldn't make a socket for " + config.server);
        Publish(session);
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
        failure = Refused(error) ? config.server + " refused the connection"
                                 : "couldn't connect to " + config.server;
    } else {
        const auto deadline = Session::Clock::now() + kConnectTimeout;
        while (!stop_) {
            const Ready ready = Wait(s, true, kWake);
            if (ready == Ready::kError) {
                failure = "couldn't connect to " + config.server;
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
                    failure = Refused(outcome) ? config.server + " refused the connection"
                                               : "no answer from " + config.server;
                }
                break;
            }
            if (Session::Clock::now() >= deadline) {
                failure = "no answer from " + config.server;
                break;
            }
        }
    }
    if (!connected) {
        CloseSocket(s);
        if (!stop_) {
            session.Failed(failure);
            Publish(session);
        }
        return;
    }

    bool open = SendAll(s, session.Connected(Session::Clock::now()));
    if (!open) session.Closed();
    Publish(session);
    uint8_t buffer[1024];
    while (!stop_ && !session.Over()) {
        const Ready ready = Wait(s, false, kWake);
        const auto now = Session::Clock::now();
        if (ready == Ready::kError) {
            session.Closed();
        } else if (ready == Ready::kYes) {
            const int n = static_cast<int>(recv(s, reinterpret_cast<char*>(buffer), sizeof(buffer), 0));
            if (n > 0) {
                if (!SendAll(s, session.Received(buffer, static_cast<size_t>(n), now))) session.Closed();
            } else if (n == 0 || !InProgress(LastError())) {
                session.Closed();
            }
        }
        for (const std::string& code : TakeJoins()) {
            if (session.Over()) break;
            if (!SendAll(s, session.Join(code))) session.Closed();
        }
        session.Tick(now);
        Publish(session);
    }
    CloseSocket(s);
}

}  // namespace band3::rooms
