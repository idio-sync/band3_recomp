#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xsocket.h>
#include <rex/types.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include "generated/band3_init.h"
#include "src/Net/events.h"
#include "src/Net/liveless_rooms.h"
#include "src/Net/liveless_rooms_client.h"
#include "src/Net/local_address.h"
#include "src/Net/native_socket.h"
#include "src/Net/online.h"
#include "src/Net/online_hooks.h"
#include "src/settings.h"

// liveless_rooms.h's client, wired to the game: a code the server finds a
// game for becomes an invite to it (FakeInvite, as the harness's
// liveless_invite), a player joining this game gets a NAT punch from the
// game's own socket, and the overshell's Friends button opens the Rooms panel.

extern "C" void __imp__PlatformMgr__ShowFriendsUI(PPCContext& ctx, uint8_t* base);

namespace {

using band3::rooms::Ipv4Text;

band3::rooms::Client g_client;
std::atomic<bool> g_enabled{false};
// what Start connected with, for Connect to connect again; or why it couldn't
std::mutex g_mutex;
band3::rooms::Config g_config;
std::string g_start_error;

std::atomic<uint32_t> g_game_socket{0};
std::atomic<bool> g_game_socket_seen{false};
std::atomic<uint32_t> g_advertised{0};
std::atomic<bool> g_panel_requested{false};

// a join: the game accepts an invite to the host's game, on RB3Enhanced's
// port (a Rooms server knows nothing of liveless_port)
std::string JoinGame(uint32_t address) {
    return band3::online::FakeInvite(Ipv4Text(address), band3::online::kGamePort, false);
}

// The host's side of a join, as RB3Enhanced's: four bytes from the game's
// socket to the joiner's address, so a router in front of this PC lets the
// joiner's packets in as answers. This thread sends on the socket Quazal
// polls; a datagram sent is all it shares with it.
void PunchTo(uint32_t address) {
    if (!g_game_socket_seen) {
        REXLOG_INFO("rooms: NAT punch asked for before the game went online, skipped");
        return;
    }
    auto socket = REX_KERNEL_OBJECTS()->LookupObject<rex::system::XSocket>(g_game_socket.load());
    if (!socket) {
        REXLOG_WARN("rooms: NAT punch to {}: the game's socket is gone", Ipv4Text(address));
        return;
    }
    static constexpr uint8_t kPunch[] = {'N', 'A', 'T', '!'};
    constexpr uint16_t kPort = band3::online::kGamePort;
    // SendTo takes the port in network order
    const uint16_t port = static_cast<uint16_t>((kPort >> 8) | (kPort << 8));
    if (band3::net::SendTo(socket->native_handle(), kPunch, sizeof(kPunch), address, port) < 0) {
        REXLOG_WARN("rooms: NAT punch to {}:{} failed, error {}", Ipv4Text(address), kPort,
                    band3::net::LastSocketError());
        return;
    }
    REXLOG_INFO("rooms: NAT punch sent to {}:{}", Ipv4Text(address), kPort);
}

band3::rooms::Callbacks MakeCallbacks() {
    band3::rooms::Callbacks callbacks;
    callbacks.on_join = JoinGame;
    callbacks.on_nat_punch = PunchTo;
    callbacks.log = [](const std::string& line) { REXLOG_INFO("{}", line); };
    return callbacks;
}

}  // namespace

namespace band3::rooms {

bool Enabled() { return g_enabled; }

Status GetStatus() {
    Status status;
    status.server = settings::Startup().liveless_rooms_server;
    if (g_enabled) {
        const ClientStatus client = g_client.GetStatus();
        status.state = client.state;
        status.code = client.code;
        status.public_ipv4 = client.public_ipv4;
        status.error = client.error;
        status.last_join_user = client.last_join_user;
        status.last_join_ipv4 = client.last_join_ipv4;
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_start_error.empty()) {
            status.state = State::kFailed;
            status.error = g_start_error;
        }
    }
    status.advertised_ipv4 = g_advertised;
    status.game_socket_seen = g_game_socket_seen;
    return status;
}

std::string Join(std::string code) {
    if (!g_enabled) return "Liveless Rooms isn't running";
    return g_client.Join(std::move(code));
}

std::string Connect() {
    if (!g_enabled) return "Liveless Rooms isn't running";
    Config config;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if (!g_start_error.empty()) return g_start_error;
        config = g_config;
    }
    g_client.Start(std::move(config), MakeCallbacks());
    return {};
}

uint32_t PublicAddress() { return g_client.PublicAddress(); }

void NoteGameSocket(uint32_t guest_handle) {
    g_game_socket = guest_handle;
    g_game_socket_seen = true;
}

void NoteAdvertised(uint32_t ipv4) { g_advertised = ipv4; }

void Start() {
    const std::string server = settings::Startup().liveless_rooms_server;
    const std::string username = settings::Username();
    Config config;
    config.server = server;
    // the host as typed, which the login proof signs: the server checks it
    // against the address it was given, so the port stays out of it
    const auto endpoint = online::ParseEndpoint(server, kPort);
    std::string error;
    if (endpoint) {
        config.host = endpoint->host;
        config.port = endpoint->port;
    } else {
        error = "liveless_rooms_server '" + server + "' isn't an address (host or host:port)";
    }
    config.gamertag = username.substr(0, 15);
    config.xuid = RoomsXuid(username);
    const auto local = net::ResolveIPv4(net::LocalAddress());
    config.local_ipv4 = local.empty() ? 0 : local.front();
    config.version = std::string("band3 ") + events::kBuildTag;
    const std::string lang = REXCVAR_GET(lang);
    config.language = lang.empty() ? "eng" : lang;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_config = config;
        g_start_error = error;
    }
    g_enabled = true;
    if (!error.empty()) {
        REXLOG_ERROR("rooms: {}", error);
        return;
    }
    REXLOG_INFO("rooms: logging in to {} as {}, local address {}", server, config.gamertag,
                Ipv4Text(config.local_ipv4));
    g_client.Start(std::move(config), MakeCallbacks());
}

void Stop() { g_client.Stop(); }

bool PanelRequested() { return g_panel_requested.exchange(false); }

void RequestPanel() { g_panel_requested = true; }

}  // namespace band3::rooms

// PlatformMgr::ShowFriendsUI(this, pad): the overshell's Friends button, which
// opens the guide's friends list for a player signed in to Live. There's no
// Live to have friends on; with Liveless Rooms on it opens the Rooms panel,
// where players find each other by code.
extern "C" REX_FUNC(PlatformMgr__ShowFriendsUI) {
    if (!band3::rooms::Enabled()) return __imp__PlatformMgr__ShowFriendsUI(ctx, base);
    REXLOG_INFO("rooms: the Friends button opens the Rooms panel");
    band3::rooms::RequestPanel();
    ctx.r3.u64 = 0;
}
