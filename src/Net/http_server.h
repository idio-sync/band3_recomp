#pragma once
#include <cstdint>

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
void StopServer();

// Runs the game work requests are waiting on. App::DrawRegular calls it once
// a frame, on the game thread.
void RunGameJobs(PPCContext& ctx, uint8_t* base);

}
