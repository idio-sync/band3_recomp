#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "src/Video/sync_align.h"

// Auto-sync: the first time a song with a music video plays, its audio's
// first kCaptureSeconds (the game's own, as it decodes it: src/Hooks/
// song_audio.cpp) are matched against the video's soundtrack
// (sync_align.h), and a sure match is saved as video_start_time in the
// video's .ini and applied at once. An .ini that already sets one is left
// alone. Both soundtracks' envelopes are kept beside the videos
// (sync_cache.h), for the Videos tab to align again without the song.

namespace band3::video {

inline constexpr double kCaptureSeconds = 45.0;

// For the song stream hook, on the game's audio thread. Wanted: whether the
// song now playing wants its audio (it has a music video, auto-sync's on,
// and none's been taken this song), cheap enough to ask every block; and
// SongGeneration, a number each song changes. Begin: its audio starts (its
// sample 0) at `rate` Hz; then each block, mixed to mono, in order.
// Aligns `video` with its song's captured sound (its .song.env, or
// `song_env` if given), the video's soundtrack read or taken from its cache,
// and saves the result (sync_cache.h's ResultPath). With `save_offset`, a
// confident one is also written to the .ini. None, with why in `error`,
// without the song's sound or the video's soundtrack.
std::optional<SyncResult> AlignVideo(const std::filesystem::path& video,
                                     std::span<const float> song_env, bool save_offset,
                                     std::string& error);

bool SongAudioWanted();
uint64_t SongGeneration();
void SongAudioBegin(uint64_t generation, int rate);
void SongAudio(uint64_t generation, std::span<const float> mono);

}
