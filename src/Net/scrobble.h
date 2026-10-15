#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

// Scrobbling: the songs played told to Last.fm (lastfm_enabled) and
// ListenBrainz (listenbrainz_enabled), as "now playing" when a song starts and
// as a listen when it ends, if it was played long enough (ShouldScrobble in
// scrobble_protocol.h). All the HTTP runs on a background thread; the game
// thread only hands songs over. Listens that couldn't be sent are kept, in
// memory, and sent with the next.
//
// Last.fm needs the player's permission once: with no lastfm_session, band3
// opens Last.fm's page in the browser, waits for the player to allow it, and
// saves the session it gets to band3.toml.

namespace band3::scrobble {

// starts the worker when either is turned on; `config_path` is band3.toml,
// where a new Last.fm session is saved
void Start(const std::filesystem::path& config_path);

// sends what's waiting (briefly) and stops the worker; safe to call more than once
void Stop();

// true once Start() has launched the worker
bool Enabled();

// from the game thread (rb3e_events.cpp): a song starting ends the one before
void SongStarted(const std::string& title, const std::string& artist, int32_t length_ms);
void SongEnded();

}
