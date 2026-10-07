#pragma once
#include <string>
#include <string_view>
#include <vector>
#include "package_scan.h"
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
    // packages set aside, or to be at the next launch (package_scan.h), in
    // the content folders as last read, and since by the calls below
    std::vector<SetAsideFile> set_aside;
};

// what's been read, starting a read when there's none and the packages changed
PackageSongsSnapshot CurrentPackageSongs();

// Sets a package aside at the next launch (RequestSetAside): only one of those
// read, with one song, so a pack's other songs never go with it. "" or why not.
std::string SetAsidePackage(std::string_view file);
// sets aside every package that's one song the game leaves out, its song_id
// being another's it loads first; how many, or why not
std::string SetAsideLeftOut(size_t& count);
// puts back one of the set_aside files: "" or why not
std::string PutBackPackage(std::string_view file);

}
