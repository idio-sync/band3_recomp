#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include "src/Net/events.h"

// Discord Rich Presence over Discord's local IPC (the discord-ipc named pipe on
// Windows, a unix socket elsewhere). All Discord I/O runs on a background thread,
// so the game thread only ever hands over the latest state.

namespace band3::discord {

// true once Start() has launched the worker ([discord] enabled at startup)
bool Enabled();

// starts the worker when [discord] enabled is set, showing the menus presence
void Start();

// clears the presence and stops the worker
void Stop();

void SetMenus();
void SetPlaying(std::string title, std::string artist, const events::BandInfo& band);

// formatting, exposed for testing
namespace detail {

struct Presence {
    bool playing = false;
    std::string title;
    std::string artist;
    std::string band;    // e.g. "Guitar (Expert) · Drums (Hard)"
    int64_t start = 0;   // unix seconds when the song started
};

// valid UTF-8 is kept; anything else is treated as Latin-1 and converted
std::string ToUtf8(std::string_view text);

// cuts to at most max_chars code points without splitting a UTF-8 sequence
std::string Truncate(std::string_view utf8, size_t max_chars);

std::string BandText(const events::BandInfo& band);

// SET_ACTIVITY command; clear = true sends no activity, which removes the presence
std::string ActivityJson(const Presence& presence, uint32_t pid, uint64_t nonce, bool clear = false);

// IPC frame: little-endian opcode and length, then the JSON payload
std::string Frame(uint32_t opcode, std::string_view json);

}

}
