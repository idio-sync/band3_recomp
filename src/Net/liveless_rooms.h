#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// band3's Liveless Rooms client, as the rest of band3 sees it: the game's
// hooks, the Rooms panel, the test server and online::Start. With
// liveless_rooms on, band3 logs in to RB3Enhanced's Rooms server for a code,
// and a player joins another by theirs instead of typing an address. The
// connection runs on a thread of its own; these are safe from any thread, and
// the ones the game thread calls never wait on it. Implemented in
// src/Hooks/liveless_rooms.cpp; the packets are liveless_rooms_protocol.h's.
namespace band3::rooms {

enum class State {
    kOff,           // liveless_rooms off, or not started
    kConnecting,    // looking up and connecting to the server
    kConnected,     // connected, logging in
    kLoggedIn,      // has a code; joins can go
    kDisconnected,  // was logged in, and the connection went
    kFailed,        // never got as far as logging in
};

// As the test server's rooms_status and rooms= wait say it.
inline std::string_view StateName(State state) {
    switch (state) {
        case State::kOff: return "off";
        case State::kConnecting: return "connecting";
        case State::kConnected: return "connected";
        case State::kLoggedIn: return "logged_in";
        case State::kDisconnected: return "disconnected";
        case State::kFailed: return "failed";
    }
    return "off";
}

// IPv4 addresses in network order, 0 for none.
struct Status {
    State state = State::kOff;
    std::string server;        // liveless_rooms_server, as typed
    std::string code;          // this player's, once logged in
    uint32_t public_ipv4 = 0;  // this PC's address as the server saw it
    // the address XNetGetTitleXnAddr last told the game is this PC's, which
    // players joining it dial
    uint32_t advertised_ipv4 = 0;
    std::string error;         // the last thing that went wrong, or empty
    // seconds until it connects again by itself, after the connection went
    // (state disconnected or failed); 0 when it won't
    int retry_in_s = 0;
    int attempt = 0;  // which connection this is since Start or Connect, from 1
    // the last game a join went to
    std::string last_join_user;
    uint32_t last_join_ipv4 = 0;
    // whether the game's online socket (NetDll_bind's) is open: it's gone
    // online, and not left Play on Xbox Live since. A join and a NAT punch
    // both need it.
    bool game_socket_seen = false;
};

// Whether liveless_rooms is on and Start ran. Any thread; the game thread's
// Friends-button hook asks it.
bool Enabled();

// A copy of the client's status. Any thread (the panel's, the test server's).
Status GetStatus();

// Joins the game with `code` (any case; 8 letters and digits) once the server
// answers. Returns an error (one is the game not being online), or empty when
// the request went. Any thread (the panel's, the test server's); doesn't wait
// for the server.
std::string Join(std::string code);

// Connects again after a disconnect or a failure, now rather than when the
// client would by itself. Returns an error, or empty. The panel's or the test
// server's thread; waits for the old connection's thread to end.
std::string Connect();

// This PC's public address as the server saw it, 0 until logged in. The game
// thread's XNetGetTitleXnAddr hook calls it, so it never locks.
uint32_t PublicAddress();

// The game thread's hooks: the guest socket handle the game bound its online
// port with (NetDll_bind), which a NAT punch sends from, and the address
// XNetGetTitleXnAddr gave the game, for GetStatus.
void NoteGameSocket(uint32_t guest_handle);
void NoteAdvertised(uint32_t ipv4);

// Start: from online::Start, before the game runs, when liveless_rooms is on
// and allowed. Stop: from the app's shutdown, before the test server stops
// and the kernel goes; waits for the connection's thread to end.
void Start();
void Stop();

// The overshell's Friends button opens the Rooms panel: its hook (the game
// thread) calls RequestPanel, and the panel (the UI thread) takes the request
// with PanelRequested, which clears it.
bool PanelRequested();
void RequestPanel();

}  // namespace band3::rooms
