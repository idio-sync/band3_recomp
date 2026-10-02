// band3 serves DLC and custom songs from the content_folders setting's folders:
// RB3 lists them with its own content and opens each package file where it is,
// so nothing is installed or unpacked (see content_hooks.cpp)
#pragma once
#include <cstdint>
#include <string_view>
#include <vector>
#include "package_scan.h"

struct PPCContext;

namespace rex::filesystem {
class VirtualFileSystem;
}

namespace band3::content {

// scans the content_folders setting's folders on a worker thread; call once,
// before the game starts
void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs);

// band3's packages, for RB3's listings, which this counts as the game's having
// listed them: calls wait for the scan until a minute after the first call;
// after that none waits, and one made while the scan is still running gets
// none (the first with a warning)
std::vector<Package> LivePackages();

// the package named file_name (its content ID), if it is one of band3's and the
// scan is done
const Package* FindLivePackage(std::string_view file_name);

// Scans the folders again for packages added since, e.g. downloaded
// (src/Net/song_downloads.h): how many it found. Packages are only added, so
// one taken away stays listed until the next launch. Waits for the folders, so
// not on the game thread.
size_t RescanLiveContent();

// a rescan has found packages the game hasn't listed yet
bool GameMissesPackages();

// Each frame, on the game thread: has the game list packages it misses where
// it's safe to (content_refresh.h). XboxContentMgr::StartRefresh's override
// (content_hooks.cpp) then has it list them, as it does after a storage change.
void PollRefresh(PPCContext& ctx, uint8_t* base);

// mounts package as root_name: (as XamContentCreateEx does); false if it won't mount
bool MountLivePackage(const Package& package, std::string_view root_name);

// unmounts root_name: if band3 mounted it; false if it's not band3's
bool UnmountLiveRoot(std::string_view root_name);

}  // namespace band3::content
