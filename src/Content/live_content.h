// band3 serves DLC and custom songs from the content_folders setting's folders:
// RB3 lists them with its own content and opens each package file where it is,
// so nothing is installed or unpacked (see content_hooks.cpp)
#pragma once
#include <string_view>
#include <vector>
#include "package_scan.h"

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace band3::content {

// scans the content_folders setting's folders on a worker thread; call once,
// before the game starts
void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs);

// band3's packages, for RB3's listings: calls wait for the scan until a minute after the first call;
// after that none waits, and one made while the scan is still running gets
// none (the first with a warning)
std::vector<Package> LivePackages();

// the package named file_name (its content ID), if it is one of band3's and the
// scan is done
const Package* FindLivePackage(std::string_view file_name);

// Scans the folders again for packages added since, e.g. downloaded
// (src/Net/song_downloads.h): how many it found. Packages are only added, so
// one taken away stays listed until the next launch. Waits for the folders, so
// not on the game thread. New packages are announced to the game as the
// console announced content it installed (XN_LIVE_CONTENT_INSTALLED), and the
// game takes them in at its next refresh, as it took in songs bought from the
// store: at once in the Music Library, after a song, and on the way into the
// library from the main hub.
size_t RescanLiveContent();

// mounts package as root_name: (as XamContentCreateEx does); false if it won't mount
bool MountLivePackage(const Package& package, std::string_view root_name);

// unmounts root_name: if band3 mounted it; false if it's not band3's
bool UnmountLiveRoot(std::string_view root_name);

}  // namespace band3::content
