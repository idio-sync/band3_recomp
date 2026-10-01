// band3 serves DLC and custom songs from the content_folders setting's folders:
// RB3 lists them with its own content and opens each package file where it is,
// so nothing is installed or unpacked (see content_hooks.cpp)
#pragma once
#include <chrono>
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

// band3's packages, once the scan is done (waits for it, up to `timeout`;
// empty if it isn't done by then)
const std::vector<Package>& LivePackages(std::chrono::milliseconds timeout);

// the package named file_name (its content ID), if it is one of band3's
const Package* FindLivePackage(std::string_view file_name);

// mounts package as root_name: (as XamContentCreateEx does); false if it won't mount
bool MountLivePackage(const Package& package, std::string_view root_name);

// unmounts root_name: if band3 mounted it; false if it's not band3's
bool UnmountLiveRoot(std::string_view root_name);

}  // namespace band3::content
