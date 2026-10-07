#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "package_scan.h"
#include "song_dta.h"

// What's in each of band3's packages (package_songs.h reads them), and the
// duplicate songs among them and the game's own, for the web page's
// /library/duplicates. How the game takes duplicates (tried 2026-10-07): of
// songs with the same song_id it has the first it loads, in band3's order of
// packages, and ignores the rest, saying nothing; songs with the same
// shortname and different song_ids it has them all, though anything that finds
// a song by its shortname finds only one.

namespace band3::content {

struct PackageSongs {
    std::string path;      // UTF-8
    int64_t size = 0;
    int64_t modified = 0;  // its last write, as the file system counts it
    std::vector<DtaSong> songs;
    bool unreadable = false;  // its songs.dta couldn't be read
};

// The cache of what was read, so a package is read again only when its size
// or time changes: {"version": 1, "packages": [{"path":, "size":, "modified":,
// "unreadable":, "songs": [{"shortname":, "song_id":, "title":, "artist":}]}]}.
// What it can't read is left out.
std::vector<PackageSongs> ParseInventoryCache(std::string_view json);
std::string FormatInventoryCache(const std::vector<PackageSongs>& packages);

// a song the game has, as /list_songs gives it, with its ID
struct GameSong {
    std::string shortname;
    int32_t song_id = 0;
    std::string title;
    std::string artist;
};

struct SongCopy {
    DtaSong song;
    // its package's index; -1 for a song in the game's own data, or a copy band3 left out
    int32_t package = -1;
    bool in_use = true;  // the game has this copy
    std::string file;    // its package (UTF-8); "" for the game's own
    int64_t size = 0;
    size_t songs_in_file = 0;
    bool differs = false;  // a copy band3 left out whose size isn't the listed one's
};

enum class DuplicateKind {
    kSameFile,   // copies of one package (its content ID): band3 lists only the first
    kSongId,     // the same song_id: the game has only the first it loads
    kShortname,  // the same shortname, other song_ids: the game has them all
    kSimilar,    // the same artist and title, by letters and digits: other charts, maybe
};

// a copy of one of the packages that band3 left out (package_scan.h's
// DroppedPackage), with its size
struct LeftOutCopy {
    std::string path;  // UTF-8
    int64_t size = 0;
    std::string kept;  // the package listed in its place
};

struct DuplicateGroup {
    DuplicateKind kind = DuplicateKind::kSongId;
    std::string key;  // the song_id, the shortname, or "artist - title"
    std::vector<SongCopy> copies;
};

// the duplicates among packages (in the order the game takes them in), the
// copies of them band3 left out, and the game's songs that aren't in any of
// them (on the disc, or in its own data); with no game songs, those on the
// disc go unseen. Same files first, then song_ids, shortnames and the
// similar, each by the first copy's title. A same file group's song is its
// package's first.
std::vector<DuplicateGroup> FindDuplicates(const std::vector<PackageSongs>& packages,
                                           const std::vector<GameSong>& game,
                                           const std::vector<LeftOutCopy>& left_out = {});

struct InventoryStatus {
    bool reading = false;  // the packages are being read
    size_t read = 0;       // of them so far
    size_t total = 0;
    bool game = false;     // the game said what songs it has
};

// /library/duplicates: {"reading":, "read":, "total":, "unreadable": packages
// whose songs couldn't be read, "game":, "groups": [{"kind": "same_file",
// "song_id", "shortname" or "similar", "key":, "copies": [{"shortname":,
// "song_id":, "title":, "artist":, "file": the package's path ("" for the
// game's own), "songs_in_file":, "size":, "in_use":, "differs":}]}],
// "set_aside": [{"file": its own path, "next_launch": not done yet}]}
std::string FormatDuplicates(const std::vector<DuplicateGroup>& groups,
                             const std::vector<PackageSongs>& packages,
                             const InventoryStatus& status,
                             const std::vector<SetAsideFile>& set_aside = {});

}
