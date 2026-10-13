#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/Content/song_inventory.h"

// A song's rhythm envelope (sync_align.h) from its audio in its package, or
// for the game's own songs its archive, so its music video can be synced
// without the song being played first. Encrypted moggs are decrypted
// (mogg_crypt.h); for a song found in neither, or encrypted in a way band3
// can't decrypt, auto-sync still takes the song's sound as it plays.

namespace band3::video {

// where a song's audio is: its package's file, and its .mogg inside, with
// backslashes as the package device takes it ("songs\\x\\x.mogg")
struct SongMogg {
    std::string package;  // UTF-8
    std::string mogg;
};

// The first package (in the order the game takes them) with a song of this
// shortname, matched without minding case as video files are; its mogg from
// the song's path, or songs/<shortname>/<shortname>.mogg without one.
std::optional<SongMogg> FindSongMogg(const std::vector<content::PackageSongs>& packages,
                                     std::string_view shortname);

// What came of looking for a song's audio
enum class SongAudioResult {
    kOk,
    kNotFound,   // in no package band3 knows of (a new one), nor the game's archive
    kEncrypted,  // its mogg is encrypted in a way band3 can't decrypt
    kFailed,     // it couldn't be read or decoded: why in `error`
};

// How much of a song's audio is read from its package: all of it (up to
// this), for a segment map over the whole song (sync_align.h's
// AlignSegments), which reading costs about a second for a long song. A
// single offset from more than auto-sync's capture (kCaptureSeconds) is surer
// too: Master of Puppets is unsure at 45 s, sure at 180, at the same offset.
inline constexpr double kPackageSongSeconds = 1200.0;

// The song's envelope from its package (song_audio_package.cpp): the
// packages band3 has read (package_songs.h's PackageSongsNow, waiting up to
// 20 s for a first read; before the game boots, those of its last run), then
// the game's archive under game_data_root (the title update's first), the
// first kPackageSongSeconds of the song's mogg. Not on a thread that mustn't
// wait.
SongAudioResult PackageSongEnvelope(std::string_view shortname, std::vector<float>& envelope,
                                    std::string& error);

}
