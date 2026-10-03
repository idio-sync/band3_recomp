#include <atomic>
#include <cstdint>
#include <string>
#include "src/Net/liveless_rooms.h"
#include "src/settings.h"

// liveless_rooms.h's client, until the connection is written: always off. The
// hooks' notes are kept so GetStatus already shows what the game did.

namespace {

std::atomic<uint32_t> g_game_socket{0};
std::atomic<bool> g_game_socket_seen{false};
std::atomic<uint32_t> g_advertised{0};
std::atomic<bool> g_panel_requested{false};

}  // namespace

namespace band3::rooms {

bool Enabled() { return false; }

Status GetStatus() {
    Status status;
    status.server = settings::Startup().liveless_rooms_server;
    status.advertised_ipv4 = g_advertised;
    status.game_socket_seen = g_game_socket_seen;
    return status;
}

std::string Join(std::string) { return "Liveless Rooms isn't running"; }

std::string Connect() { return "Liveless Rooms isn't running"; }

uint32_t PublicAddress() { return 0; }

void NoteGameSocket(uint32_t guest_handle) {
    g_game_socket = guest_handle;
    g_game_socket_seen = true;
}

void NoteAdvertised(uint32_t ipv4) { g_advertised = ipv4; }

void Start() {}

void Stop() {}

bool PanelRequested() { return g_panel_requested.exchange(false); }

void RequestPanel() { g_panel_requested = true; }

}  // namespace band3::rooms
