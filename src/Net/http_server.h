#pragma once
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

struct PPCContext;

// RB3Enhanced's web server: with http_enabled set, band3 serves a web page and
// RB3E's endpoints (http_request.h) on http_port, to this machine and the local
// network, so a phone or another PC can browse the song library and pick the
// next song.

namespace band3::http {

// Starts listening when http_enabled was set at startup.
void StartServer();
// http_enabled was set at startup, so the hooks keep what /status reports
bool Enabled();

// The address (http_address) and port the server listens on while it does;
// nullopt before it starts, when it couldn't, and once it stops. Any thread.
struct Listening {
    std::string address;
    int port = 0;
};
std::optional<Listening> ListeningOn();
void StopServer();

// Runs the game work requests are waiting on. App::DrawRegular calls it once
// a frame, on the game thread.
void RunGameJobs(PPCContext& ctx, uint8_t* base);

// Runs `job` on the game thread at its next frame (RunGameJobs, which runs
// whether the server's on or not) and waits for it; false if the game didn't
// get to it within a few seconds, or band3 is closing. Not from the game thread.
bool RunOnGameThreadAndWait(std::function<void(PPCContext&, uint8_t*)> job);

}
