#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

#include "src/Video/sync_align.h"

// Auto-sync: when a song with a music video is picked, its audio's first
// kCaptureSeconds are matched against the video's soundtrack (sync_align.h):
// read from its package where it can be (song_audio.h), else taken as the
// song plays (the game's own audio, as it decodes it: src/Hooks/
// song_audio.cpp). A sure match is saved as video_start_time in the video's
// .ini and applied at once. An .ini that already sets one is left alone. Both
// soundtracks' envelopes are kept beside the videos (sync_cache.h), for the
// Videos tab to align again without the song.

namespace band3::video {

inline constexpr double kCaptureSeconds = 45.0;
// A song whose sound has to be taken as it plays (an encrypted one, the
// disc's) is taken whole, up to this: its first kCaptureSeconds give one
// offset at once, and at its end (EndSongCapture) the whole of it a segment
// map, which takes the place of the offset auto-sync wrote.
inline constexpr double kMaxCaptureSeconds = 1200.0;

// For the song stream hook, on the game's audio thread. Wanted: whether the
// song now playing wants its audio (it has a music video, auto-sync's on,
// and none's been taken this song), cheap enough to ask every block; and
// SongGeneration, a number each song changes. Begin: its audio starts (its
// sample 0) at `rate` Hz; then each block, mixed to mono, in order.
// Aligns `video` with its song's sound (`song_env` if given, else its
// .song.env, else read from its package and kept there), the video's
// soundtrack read or taken from its cache (sync_align.h's MatchSong: a
// segment map over the whole song, one offset over a capture's 45 s), and
// saves the result (sync_cache.h's ResultPath). With `save_offset`, a
// confident one is also written to the .ini (WriteMatch). None, with why in
// `error`, without the song's sound or the video's soundtrack.
std::optional<Match> AlignVideo(const std::filesystem::path& video, std::span<const float> song_env,
                                bool save_offset, std::string& error);

// The song's video has just been picked (SetMusicVideoSong): without a start
// time, it's aligned on a thread of its own from its song's package, before
// the song plays; unsure or without it, the capture goes on as before.
void SyncBeforePlaying();

// The song's ended or is leaving (SetMusicVideoSong, before the next is
// picked): the sound taken of it, if there's enough (kMinMapSeconds), is
// matched whole on a thread of its own.
void EndSongCapture();

bool SongAudioWanted();
uint64_t SongGeneration();
void SongAudioBegin(uint64_t generation, int rate);
void SongAudio(uint64_t generation, std::span<const float> mono);

}
