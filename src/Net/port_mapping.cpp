#include "port_mapping.h"
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include "gateway_address.h"
#include "native_socket.h"
#include "online.h"
#include "port_mapping_protocol.h"

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
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <miniupnpc.h>
#include <upnpcommands.h>
#include <upnperrors.h>

namespace band3::port_mapping {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// what band3 asks for, as RB3Enhanced: an hour, renewed at half of what the
// router grants
constexpr uint32_t kLifetime = 3600;
// a PCP or NAT-PMP request goes again this long after the first while
// nothing answers (RFC 6886's doubling from 250 ms, cut short), and a router
// silent for kGiveUp doesn't speak it
constexpr std::chrono::milliseconds kResends[] = {250ms, 500ms, 1000ms, 2000ms};
constexpr auto kGiveUp = 3500ms;
// deleting the mapping at Stop
constexpr auto kDeleteBudget = 1000ms;
// how long Stop waits for the thread: the delete, and a UPnP call already
// under way, which miniupnpc doesn't let band3 cut short
constexpr auto kStopWait = 3000ms;
// how often a wait on the router's answer looks at Stop
constexpr auto kSlice = 50ms;
// how long SSDP discovery waits for routers to answer
constexpr int kDiscoverMs = 2000;
// UPnP's OnlyPermanentLeasesSupported and ConflictInMappingEntry
constexpr int kUpnpPermanentOnly = 725;
constexpr int kUpnpConflict = 718;
// what band3 calls its UPnP mappings, to know one again
constexpr char kUpnpDescription[] = "band3";
// the shortest wait before renewing, whatever lease a router grants
constexpr uint32_t kMinRenew = 30;

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif

std::string Ipv4Text(uint32_t address) {
    uint8_t b[4];
    std::memcpy(b, &address, 4);
    return std::to_string(b[0]) + "." + std::to_string(b[1]) + "." + std::to_string(b[2]) + "." +
           std::to_string(b[3]);
}

// What the thread and everyone else share. The thread holds it too, so a
// thread Stop stopped waiting for still has it.
struct Shared {
    std::mutex mutex;
    std::condition_variable wake;
    bool stop = false;
    bool finished = false;
    Status status;
    // Stop gave up waiting: the process is on its way out, and the log may go
    // before the thread does
    std::atomic<bool> abandoned{false};
};

std::mutex g_control;  // Start and Stop, one at a time
std::thread g_thread;
std::shared_ptr<Shared> g_shared;
// ExternalAddress's, for the game thread
std::atomic<uint32_t> g_external{0};

// One run of the thread: map the port, renew it, delete it at Stop.
class Worker {
public:
    Worker(Config config, std::shared_ptr<Shared> shared)
        : config_(std::move(config)), shared_(std::move(shared)) {
        // PCP knows the mapping by this, to renew and delete it
        std::random_device random;
        for (uint8_t& b : nonce_) b = static_cast<uint8_t>(random());
    }

    ~Worker() { Reset(); }

    void Run() {
#ifdef _WIN32
        // miniupnpc leaves Winsock to its caller; the count keeps the SDK's
        WSADATA wsa;
        const bool started = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
#endif
        bool mapped = Map();
        while (mapped) {
            // a permanent mapping (UPnP's lease 0) needs no renewing
            const auto renew = std::chrono::seconds(std::max(lease_ / 2, kMinRenew));
            if (lease_ == 0 ? WaitForStop() : WaitUntil(Clock::now() + renew)) break;
            std::string error;
            if (Renew(error)) continue;
            Log("port mapping: renewing UDP " + std::to_string(config_.port) + " failed (" + error +
                "); asking again");
            mapped = Map();
        }
        if (mapped) {
            Delete();
        } else {
            WaitForStop();
        }
        Reset();
#ifdef _WIN32
        if (started) WSACleanup();
#endif
        std::lock_guard<std::mutex> lock(shared_->mutex);
        shared_->finished = true;
        shared_->wake.notify_all();
    }

private:
    enum class Answer { kReply, kSilent, kStopped };

    void Log(const std::string& line) {
        if (!shared_->abandoned) REXLOG_INFO("{}", line);
    }

    void Warn(const std::string& line) {
        if (!shared_->abandoned) REXLOG_WARN("{}", line);
    }

    bool Stopping() {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        return shared_->stop;
    }

    // true if Stop came first
    bool WaitUntil(Clock::time_point until) {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        return shared_->wake.wait_until(lock, until, [&] { return shared_->stop; });
    }

    bool WaitForStop() {
        std::unique_lock<std::mutex> lock(shared_->mutex);
        shared_->wake.wait(lock, [&] { return shared_->stop; });
        return true;
    }

    void Publish(State state, std::string error = {}) {
        std::lock_guard<std::mutex> lock(shared_->mutex);
        Status& status = shared_->status;
        status.state = state;
        status.method = method_;
        status.external_ipv4 = external_;
        status.port = config_.port;
        status.lease_s = state == State::kMapped ? lease_ : 0;
        status.error = std::move(error);
    }

    std::string Port() const { return "UDP " + std::to_string(config_.port); }

    // Each way the harness guard allows, until one maps the port. True when
    // it's mapped; false when none did, or Stop came.
    bool Map() {
        Reset();
        const bool gateway = !config_.harness || !config_.gateway.empty();
        const bool upnp = !config_.harness || !config_.upnp_url.empty();
        std::string error;
        if (gateway && MapWithGateway(error)) return true;
        if (Stopping()) return false;
        if (upnp) {
            std::string upnp_error;
            if (MapWithUpnp(upnp_error)) return true;
            if (Stopping()) return false;
            error += (error.empty() ? "" : "; ") + upnp_error;
        }
        Publish(State::kFailed, error);
        Log("port mapping: " + Port() + " not mapped: " + error);
        return false;
    }

    void Reset() {
        if (socket_ != kNoSocket) CloseSocket(socket_);
        socket_ = kNoSocket;
        if (have_igd_) FreeUPNPUrls(&urls_);
        have_igd_ = false;
        method_ = Method::kNone;
        lease_ = 0;
        external_ = 0;
        g_external = 0;
    }

    // PCP and NAT-PMP: a UDP socket connected to the router, so only it
    // answers, and its own address as the router sees it, which PCP sends
    bool OpenGatewaySocket(std::string& error) {
        uint16_t port = kGatewayPort;
        if (!config_.gateway.empty()) {
            const auto endpoint = online::ParseEndpoint(config_.gateway, kGatewayPort);
            if (!endpoint) {
                error = "liveless_gateway '" + config_.gateway + "' isn't an address (host or host:port)";
                return false;
            }
            const auto found = net::ResolveIPv4(endpoint->host);
            if (found.empty()) {
                error = "no IPv4 address for liveless_gateway " + endpoint->host;
                return false;
            }
            gateway_ = found.front();
            port = endpoint->port;
        } else {
            gateway_ = net::DefaultGateway();
            if (!gateway_) {
                error = "no router found (this PC has no default gateway)";
                return false;
            }
        }
        where_ = Ipv4Text(gateway_) + ":" + std::to_string(port);
        socket_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = gateway_;
        sockaddr_in local{};
        socklen_t length = sizeof(local);
        if (socket_ == kNoSocket ||
            connect(socket_, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) != 0 ||
            getsockname(socket_, reinterpret_cast<sockaddr*>(&local), &length) != 0) {
            error = "can't reach the router at " + where_ + " (error " +
                    std::to_string(net::LastSocketError()) + ")";
            return false;
        }
        client_ipv4_ = local.sin_addr.s_addr;
        return true;
    }

    void Send(const uint8_t* data, size_t size) {
        send(socket_, reinterpret_cast<const char*>(data), static_cast<int>(size), 0);
    }

    // A datagram from the router within `wait`: its size, 0 for none, -1
    // when nothing listens there (an ICMP port unreachable from an earlier
    // send) or the socket failed.
    int Receive(uint8_t* buffer, size_t size, std::chrono::milliseconds wait) {
        fd_set readable;
        FD_ZERO(&readable);
        FD_SET(socket_, &readable);
        timeval timeout{0, static_cast<long>(wait.count() * 1000)};
#ifdef _WIN32
        const int ready = select(0, &readable, nullptr, nullptr, &timeout);
#else
        const int ready = select(socket_ + 1, &readable, nullptr, nullptr, &timeout);
#endif
        if (ready <= 0) return 0;
        const int received = static_cast<int>(
            recv(socket_, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0));
        if (received >= 0) return received;
#ifdef _WIN32
        // longer than any reply band3 reads: cut short, and read as one
        if (WSAGetLastError() == WSAEMSGSIZE) return static_cast<int>(size);
#endif
        return -1;
    }

    // Sends `request` until a reply `wanted` takes comes, resending on
    // kResends' schedule. kStopped only when `stoppable`: the delete runs
    // after Stop.
    Answer Ask(const uint8_t* request, size_t size, const std::function<bool(const Reply&)>& wanted,
               Reply& reply, Clock::duration give_up, bool stoppable = true) {
        const auto start = Clock::now();
        size_t resent = 0;
        Send(request, size);
        for (;;) {
            if (stoppable && Stopping()) return Answer::kStopped;
            const auto elapsed = Clock::now() - start;
            if (elapsed >= give_up) return Answer::kSilent;
            if (resent < std::size(kResends) && elapsed >= kResends[resent]) {
                Send(request, size);
                resent++;
            }
            uint8_t buffer[128];
            const int received = Receive(buffer, sizeof(buffer), kSlice);
            if (received < 0) return Answer::kSilent;
            if (received == 0) continue;
            reply = ParseReply(buffer, static_cast<size_t>(received));
            if (wanted(reply)) return Answer::kReply;
        }
    }

    Answer AskPcp(uint32_t lifetime, Reply& reply, Clock::duration give_up, bool stoppable = true) {
        const auto request = EncodePcpMap(nonce_, client_ipv4_, config_.port, lifetime);
        return Ask(request.data(), request.size(), [&](const Reply& r) {
            // a failure may not carry the nonce back
            return r.kind == Reply::Kind::kUnsupportedVersion ||
                   (r.kind == Reply::Kind::kPcpMap && (r.result != 0 || r.nonce == nonce_));
        }, reply, give_up, stoppable);
    }

    Answer AskNatPmpMap(uint32_t lifetime, Reply& reply, Clock::duration give_up,
                        bool stoppable = true) {
        // RFC 6886: a delete asks for external port 0
        const auto request = EncodeNatPmpMap(config_.port, lifetime ? config_.port : 0, lifetime);
        return Ask(request.data(), request.size(), [](const Reply& r) {
            return r.kind == Reply::Kind::kUnsupportedVersion || r.kind == Reply::Kind::kNatPmpMap;
        }, reply, give_up, stoppable);
    }

    // PCP, then NAT-PMP if the router says it doesn't speak PCP
    bool MapWithGateway(std::string& error) {
        if (!OpenGatewaySocket(error)) return false;
        method_ = Method::kPcp;
        Publish(State::kSearching);
        Log("port mapping: asking the router at " + where_ + " for " + Port() + " by PCP");
        Reply reply;
        Answer answer = AskPcp(kLifetime, reply, kGiveUp);
        if (answer == Answer::kStopped) return false;
        if (answer == Answer::kSilent) {
            error = "no answer to PCP or NAT-PMP from the router at " + where_;
            return false;
        }
        if (reply.kind == Reply::Kind::kPcpMap) {
            if (reply.result != 0) {
                error = "the router at " + where_ + " said " + PcpResultText(reply.result);
                return false;
            }
            return Granted(Method::kPcp, reply.external_port, reply.external_ipv4,
                           reply.lifetime_s, error);
        }

        method_ = Method::kNatPmp;
        Publish(State::kSearching);
        Log("port mapping: the router at " + where_ + " doesn't speak PCP; asking by NAT-PMP");
        const auto ask_address = EncodeNatPmpExternalAddress();
        answer = Ask(ask_address.data(), ask_address.size(), [](const Reply& r) {
            return r.kind == Reply::Kind::kUnsupportedVersion ||
                   r.kind == Reply::Kind::kNatPmpExternalAddress;
        }, reply, kGiveUp);
        if (answer == Answer::kStopped) return false;
        if (answer == Answer::kSilent) {
            error = "no answer to NAT-PMP from the router at " + where_;
            return false;
        }
        if (reply.kind == Reply::Kind::kUnsupportedVersion) {
            error = "the router at " + where_ + " speaks neither PCP nor NAT-PMP";
            return false;
        }
        if (reply.result != 0) {
            error = "the router at " + where_ + " said " + NatPmpResultText(reply.result);
            return false;
        }
        const uint32_t external = reply.external_ipv4;
        answer = AskNatPmpMap(kLifetime, reply, kGiveUp);
        if (answer == Answer::kStopped) return false;
        if (answer == Answer::kSilent || reply.kind != Reply::Kind::kNatPmpMap) {
            error = "no answer to NAT-PMP's mapping from the router at " + where_;
            return false;
        }
        if (reply.result != 0) {
            error = "the router at " + where_ + " said " + NatPmpResultText(reply.result);
            return false;
        }
        return Granted(Method::kNatPmp, reply.external_port, external, reply.lifetime_s, error);
    }

    bool MapWithUpnp(std::string& error) {
        method_ = Method::kUpnp;
        Publish(State::kSearching);
        if (!config_.upnp_url.empty()) {
            // straight to the description, as a router's SSDP answer would point
            Log("port mapping: asking the UPnP router at " + config_.upnp_url + " for " + Port());
            if (UPNP_GetIGDFromUrl(config_.upnp_url.c_str(), &urls_, &data_, lan_, sizeof(lan_)) != 1) {
                error = "no UPnP router description at " + config_.upnp_url;
                return false;
            }
            have_igd_ = true;
        } else {
            Log("port mapping: looking for a UPnP router for " + Port());
            int discover_error = 0;
            UPNPDev* devices = upnpDiscover(kDiscoverMs, nullptr, nullptr, UPNP_LOCAL_PORT_ANY, 0, 2,
                                            &discover_error);
            if (!devices) {
                error = "no UPnP router answered";
                return false;
            }
            char wan[64] = {};
            const int found = UPNP_GetValidIGD(devices, &urls_, &data_, lan_, sizeof(lan_), wan,
                                               sizeof(wan));
            freeUPNPDevlist(devices);
            if (found == UPNP_NO_IGD) {
                error = "no UPnP internet gateway answered";
                return false;
            }
            have_igd_ = true;
            if (found == UPNP_DISCONNECTED_IGD) {
                error = "the UPnP router isn't connected to the internet";
                return false;
            }
            if (found == UPNP_UNKNOWN_DEVICE) {
                error = "no UPnP internet gateway answered, only other UPnP devices";
                return false;
            }
        }
        if (Stopping()) return false;
        if (!urls_.controlURL || !urls_.controlURL[0] || !data_.first.servicetype[0]) {
            error = "the UPnP router has no WANIPConnection to map ports with";
            return false;
        }
        char external_text[40] = {};
        uint32_t external = 0;
        if (UPNP_GetExternalIPAddress(urls_.controlURL, data_.first.servicetype, external_text) ==
                UPNPCOMMAND_SUCCESS &&
            inet_pton(AF_INET, external_text, &external) != 1) {
            external = 0;
        }
        uint32_t lease = 0;
        if (!AddUpnp(lease, error)) return false;
        return Granted(Method::kUpnp, config_.port, external, lease, error);
    }

    bool AddUpnp(uint32_t& lease, std::string& error) {
        const std::string lifetime = std::to_string(kLifetime);
        std::string holder;
        int result = AddUpnpMapping(lifetime.c_str(), holder);
        lease = kLifetime;
        if (result == kUpnpPermanentOnly) {
            Log("port mapping: the UPnP router only maps for good; asking for that, and band3 "
                "deletes it when it closes");
            result = AddUpnpMapping("0", holder);
            lease = 0;
        }
        if (result != UPNPCOMMAND_SUCCESS) {
            const char* text = strupnperror(result);
            error = !holder.empty()
                        ? "the UPnP router forwards " + Port() + " to " + holder + " already"
                        : "the UPnP router refused the mapping: " + std::to_string(result) +
                              (text ? std::string(" ") + text : std::string());
            return false;
        }
        return true;
    }

    // One AddPortMapping. A conflict with band3's own mapping to this PC is
    // one a band3 that crashed or was killed didn't delete (a router that only
    // maps for good keeps it), which some routers won't overwrite: that's
    // deleted and asked for again, once. Another PC's or another program's
    // isn't touched; `holder` then says whose it is.
    int AddUpnpMapping(const char* lifetime, std::string& holder) {
        const std::string port = std::to_string(config_.port);
        auto add = [&] {
            return UPNP_AddPortMapping(urls_.controlURL, data_.first.servicetype, port.c_str(),
                                       port.c_str(), lan_, kUpnpDescription, "UDP", nullptr, lifetime);
        };
        const int result = add();
        if (result != kUpnpConflict) return result;
        char client[16] = {}, internal_port[6] = {}, description[80] = {}, enabled[4] = {},
             lease[16] = {};
        if (UPNP_GetSpecificPortMappingEntry(urls_.controlURL, data_.first.servicetype, port.c_str(),
                                             "UDP", nullptr, client, internal_port, description,
                                             enabled, lease) != UPNPCOMMAND_SUCCESS) {
            return result;
        }
        if (std::strcmp(client, lan_) != 0 || std::strcmp(description, kUpnpDescription) != 0) {
            holder = std::string(client[0] ? client : "another address") +
                     (description[0] ? " (\"" + std::string(description) + "\")" : std::string());
            return result;
        }
        Log("port mapping: the UPnP router has an old band3 mapping of " + Port() +
            " to this PC that it won't replace (error 718); deleting it and asking again");
        const int deleted = UPNP_DeletePortMapping(urls_.controlURL, data_.first.servicetype,
                                                   port.c_str(), "UDP", nullptr);
        if (deleted != UPNPCOMMAND_SUCCESS) {
            Log("port mapping: deleting the old mapping failed (" + std::to_string(deleted) +
                "); asking again anyway");
        }
        return add();
    }

    // The router's answer to a mapping: only the port asked for will do, as
    // RB3Enhanced has it, since players reach the game at liveless_port
    bool Granted(Method method, uint16_t external_port, uint32_t external, uint32_t lifetime,
                 std::string& error) {
        method_ = method;
        external_ = external;
        if (external_port != config_.port) {
            error = std::string(MethodLabel(method)) + " mapped it to port " +
                    std::to_string(external_port) + " outside, not " + std::to_string(config_.port);
            // let go of the mapping that's no use, without waiting to hear
            if (method == Method::kPcp) {
                const auto request = EncodePcpMap(nonce_, client_ipv4_, config_.port, 0);
                Send(request.data(), request.size());
            } else if (method == Method::kNatPmp) {
                const auto request = EncodeNatPmpMap(config_.port, 0, 0);
                Send(request.data(), request.size());
            }
            return false;
        }
        lease_ = lifetime;
        Publish(State::kMapped);
        Log("port mapping: " + Port() + " mapped by " + std::string(MethodLabel(method)) +
            ", public address " + (external ? Ipv4Text(external) : std::string("unknown")) +
            (lifetime ? ", for " + std::to_string(lifetime) + " s" : ", until band3 closes"));
        if (external && IsPrivateAddress(external)) {
            Warn("port mapping: the router's public address " + Ipv4Text(external) +
                 " is a private one (is it behind another router?); not telling players it");
        } else {
            g_external = external;
        }
        return true;
    }

    bool Renew(std::string& error) {
        Reply reply;
        Answer answer = Answer::kSilent;
        switch (method_) {
            case Method::kUpnp: {
                uint32_t lease = 0;
                if (!AddUpnp(lease, error)) return false;
                lease_ = lease;
                Publish(State::kMapped);
                return true;
            }
            case Method::kPcp: answer = AskPcp(kLifetime, reply, kGiveUp); break;
            case Method::kNatPmp: answer = AskNatPmpMap(kLifetime, reply, kGiveUp); break;
            case Method::kNone: return false;
        }
        if (answer == Answer::kStopped) {
            error = "stopping";
            return false;
        }
        const bool pcp = method_ == Method::kPcp;
        if (answer == Answer::kSilent ||
            reply.kind != (pcp ? Reply::Kind::kPcpMap : Reply::Kind::kNatPmpMap)) {
            error = "no answer from the router at " + where_;
            return false;
        }
        if (reply.result != 0) {
            error = pcp ? PcpResultText(reply.result) : NatPmpResultText(reply.result);
            return false;
        }
        if (reply.external_port != config_.port) {
            error = "the router moved it to port " + std::to_string(reply.external_port);
            return false;
        }
        lease_ = reply.lifetime_s;
        Publish(State::kMapped);
        Log("port mapping: " + Port() + " renewed by " + std::string(MethodLabel(method_)) +
            " for " + std::to_string(lease_) + " s");
        return true;
    }

    // Stop came: the router lets go of the mapping now, not when it lapses
    void Delete() {
        g_external = 0;
        bool deleted = false;
        Reply reply;
        switch (method_) {
            case Method::kPcp:
                deleted = AskPcp(0, reply, kDeleteBudget, false) == Answer::kReply &&
                          reply.kind == Reply::Kind::kPcpMap && reply.result == 0;
                break;
            case Method::kNatPmp:
                deleted = AskNatPmpMap(0, reply, kDeleteBudget, false) == Answer::kReply &&
                          reply.kind == Reply::Kind::kNatPmpMap && reply.result == 0;
                break;
            case Method::kUpnp: {
                const std::string port = std::to_string(config_.port);
                deleted = UPNP_DeletePortMapping(urls_.controlURL, data_.first.servicetype,
                                                 port.c_str(), "UDP", nullptr) == UPNPCOMMAND_SUCCESS;
                break;
            }
            case Method::kNone: return;
        }
        if (deleted) {
            Log("port mapping: " + Port() + " unmapped (" + std::string(MethodLabel(method_)) + ")");
        } else {
            Warn("port mapping: the router didn't confirm deleting " + Port() +
                 (lease_ ? "; it lapses within " + std::to_string(lease_) + " s" : std::string()));
        }
        Publish(State::kOff);
    }

    Config config_;
    std::shared_ptr<Shared> shared_;
    // PCP and NAT-PMP
    socket_t socket_ = kNoSocket;
    uint32_t gateway_ = 0;
    std::string where_;  // the router's address and port, for messages
    uint32_t client_ipv4_ = 0;
    Nonce nonce_{};
    // UPnP: the router's control URL and service, and this PC's address as it sees it
    UPNPUrls urls_{};
    IGDdatas data_{};
    char lan_[64] = {};
    bool have_igd_ = false;
    // what mapped the port, and what the router said
    Method method_ = Method::kNone;
    uint32_t lease_ = 0;
    uint32_t external_ = 0;
};

}  // namespace

void Start(const Config& config) {
    std::lock_guard<std::mutex> lock(g_control);
    if (g_shared) return;
    auto shared = std::make_shared<Shared>();
    shared->status.port = config.port;
    g_shared = shared;
    const bool gateway = !config.harness || !config.gateway.empty();
    const bool upnp = !config.harness || !config.upnp_url.empty();
    if (!gateway && !upnp) {
        // a test run never asks the real router: only the overrides' mock
        shared->status.error = "skipped under the test harness";
        REXLOG_INFO("port mapping: skipped under the test harness");
        return;
    }
    if (!gateway) REXLOG_INFO("port mapping: PCP and NAT-PMP skipped under the test harness (no liveless_gateway)");
    if (!upnp) REXLOG_INFO("port mapping: UPnP skipped under the test harness (no liveless_upnp_url)");
    shared->status.state = State::kSearching;
    g_thread = std::thread([config, shared] { Worker(config, shared).Run(); });
}

void Stop() {
    std::thread thread;
    std::shared_ptr<Shared> shared;
    {
        std::lock_guard<std::mutex> lock(g_control);
        thread = std::move(g_thread);
        shared = g_shared;
    }
    if (!thread.joinable()) return;
    bool finished = false;
    {
        std::unique_lock<std::mutex> lock(shared->mutex);
        shared->stop = true;
        shared->wake.notify_all();
        finished = shared->wake.wait_for(lock, kStopWait, [&] { return shared->finished; });
    }
    if (finished) {
        thread.join();
    } else {
        shared->abandoned = true;
        thread.detach();
        REXLOG_WARN("port mapping: the router is slow to answer; leaving the mapping to lapse");
    }
    g_external = 0;
}

Status GetStatus() {
    std::shared_ptr<Shared> shared;
    {
        std::lock_guard<std::mutex> lock(g_control);
        shared = g_shared;
    }
    if (!shared) return {};
    std::lock_guard<std::mutex> lock(shared->mutex);
    return shared->status;
}

uint32_t ExternalAddress() { return g_external; }

}  // namespace band3::port_mapping
