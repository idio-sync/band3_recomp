#include "live_content.h"
#include <algorithm>
#include <condition_variable>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/filesystem/devices/stfs_container_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xam/content_manager.h>
#include "src/config.h"
#include "src/paths.h"
#include "src/settings.h"

namespace band3::content {

namespace {

// the scan's result; leaked, so a scan still waiting on a network share when
// the game quits never writes to a destroyed object
struct Scan {
    std::mutex mutex;
    std::condition_variable cv;
    bool done = false;
    std::vector<Package> packages;
};

Scan& TheScan() {
    static Scan* scan = new Scan;
    return *scan;
}

std::mutex g_mutex;
rex::filesystem::VirtualFileSystem* g_vfs = nullptr;
// band3's mounts: lower-cased root name -> device mount path
std::map<std::string, std::string> g_mounts;
int g_next_mount = 0;

std::string Lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    });
    return out;
}

void Finish(std::vector<Package> packages) {
    auto& scan = TheScan();
    {
        std::lock_guard lock(scan.mutex);
        scan.packages = std::move(packages);
        scan.done = true;
    }
    scan.cv.notify_all();
}

#ifdef _WIN32
void RunScan(std::string setting) {
    const auto start = std::chrono::steady_clock::now();
    const auto anchor = IniAnchor();
    std::vector<std::filesystem::path> folders;
    std::string names;
    for (const auto& entry : paths::SplitList(setting)) {
        folders.push_back(paths::Resolve(entry, anchor));
        if (!names.empty()) names += " | ";
        names += rex::path_to_utf8(folders.back());
    }

    std::vector<std::string> problems;
    auto packages = ScanFolders(folders, kRb3TitleId, &problems);
    // no songs folder is how band3 ships, so only an unchanged default stays quiet
    const bool quiet = setting == "songs";
    for (const auto& problem : problems) {
        if (quiet) {
            REXLOG_DEBUG("content: {}", problem);
        } else {
            REXLOG_WARN("content: {}", problem);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    REXLOG_INFO("content: {} packages from {} in {} ms", packages.size(),
                names.empty() ? "no folders" : names, ms);
    Finish(std::move(packages));
}
#endif

// band3's mount of root_name, if any; g_mutex held
bool UnmountLocked(std::string_view root_name) {
    auto it = g_mounts.find(Lower(root_name));
    if (it == g_mounts.end()) return false;
    g_vfs->UnregisterSymbolicLink(std::string(root_name) + ":");
    g_vfs->UnregisterDevice(it->second);
    g_mounts.erase(it);
    return true;
}

}  // namespace

void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs) {
    g_vfs = vfs;
#ifdef _WIN32
    // an unreachable network share can take tens of seconds to give up on
    std::thread(RunScan, REXCVAR_GET(content_folders)).detach();
#else
    // listing packages the game then couldn't open would do no good: the
    // XamContentCreateEx override that opens them is Windows-only
    REXLOG_INFO("content: content folders need Windows for now, not reading {}",
                REXCVAR_GET(content_folders));
    Finish({});
#endif
}

const std::vector<Package>& LivePackages(std::chrono::milliseconds timeout) {
    static const std::vector<Package> kNone;
    auto& scan = TheScan();
    std::unique_lock lock(scan.mutex);
    // the packages never change once done, so the reference stays good
    return scan.cv.wait_for(lock, timeout, [&] { return scan.done; }) ? scan.packages : kNone;
}

const Package* FindLivePackage(std::string_view file_name) {
    const auto name = Lower(file_name);
    for (const auto& package : LivePackages(std::chrono::milliseconds(0))) {
        if (Lower(package.header.content_id) == name) return &package;
    }
    return nullptr;
}

bool MountLivePackage(const Package& package, std::string_view root_name) {
    // a package the SDK has open on this root goes first, as it does for CREATE_ALWAYS
    REX_KERNEL_STATE()->content_manager()->CloseContent(root_name);
    std::lock_guard lock(g_mutex);
    UnmountLocked(root_name);  // band3's own earlier mount of this root, if any
    // the trailing separator keeps \Device\Band3Content\1\ from matching ...\10\ (the VFS
    // takes the first device whose mount path is a prefix)
    const std::string mount = fmt::format("\\Device\\Band3Content\\{}\\", ++g_next_mount);
    auto device = std::make_unique<rex::filesystem::StfsContainerDevice>(mount, package.path);
    if (!device->Initialize()) {
        REXLOG_WARN("content: can't read {}", rex::path_to_utf8(package.path));
        return false;
    }
    const std::string link = std::string(root_name) + ":";
    g_vfs->UnregisterSymbolicLink(link);
    g_vfs->RegisterDevice(std::move(device));
    g_vfs->RegisterSymbolicLink(link, mount);
    g_mounts[Lower(root_name)] = mount;
    return true;
}

bool UnmountLiveRoot(std::string_view root_name) {
    std::lock_guard lock(g_mutex);
    return UnmountLocked(root_name);
}

}  // namespace band3::content
