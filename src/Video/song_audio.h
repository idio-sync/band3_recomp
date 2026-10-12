#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "src/Content/song_inventory.h"

// A song's rhythm envelope (sync_align.h) from its audio in its package, so
// its music video can be synced without the song being played first. Only
// songs in band3's packages, and only unencrypted moggs (mogg.h): for the
// rest, auto-sync still takes the song's sound as it plays.

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
    kNotFound,   // in no package band3 knows of (the disc's, or a new one)
    kEncrypted,  // its mogg is encrypted
    kFailed,     // it couldn't be read or decoded: why in `error`
};

// How much of a song's audio is read from its package: more than auto-sync
// can take as it plays (kCaptureSeconds), as reading it costs little (well
// under a second) and a song that repeats itself matches surely only over a
// longer stretch (Master of Puppets: not sure at 45 s, sure at 180, at the same
// offset). Within the video soundtrack auto-sync reads (240 s).
inline constexpr double kPackageSongSeconds = 180.0;

// The song's envelope from its package (song_audio_package.cpp): the
// packages band3 has read (package_songs.h's PackageSongsNow, waiting up to
// 20 s for a first read; before the game boots, those of its last run), the
// first kPackageSongSeconds of the song's mogg. Not on a thread that mustn't
// wait.
SongAudioResult PackageSongEnvelope(std::string_view shortname, std::vector<float>& envelope,
                                    std::string& error);

}
