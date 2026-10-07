#pragma once
#include <vector>
#include "song_inventory.h"

// The songs in band3's packages (live_content.h's LivePackages, in the order
// the game takes them in), each read from the package's songs/songs.dta on a
// thread of its own: the first time they're asked for, and again when the
// packages change. A package read before comes from the cache in the user data
// root (band3_package_songs.json) while its size and time stay the same, so
// only new and changed ones are opened.

namespace band3::content {

struct PackageSongsSnapshot {
    std::vector<PackageSongs> packages;  // as last read in full; empty before
    InventoryStatus status;              // its game field is left false
};

// what's been read, starting a read when there's none and the packages changed
PackageSongsSnapshot CurrentPackageSongs();

}
