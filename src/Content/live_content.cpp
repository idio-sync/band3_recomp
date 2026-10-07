#include "live_content.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
#include <set>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
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
    bool late = false;  // the listings' wait ran out first
    // only ever added to, so pointers to them (by_id, FindLivePackage's) stay good
    std::deque<Package> packages;
    // packages by content ID (upper-case hex, as the headers give it)
    std::unordered_map<std::string, const Package*> by_id;
    // the packages' files, so a file already listed isn't read again
    std::set<std::filesystem::path> paths;
    // copies of listed packages, left out
    std::vector<DroppedPackage> dropped;
};

Scan& TheScan() {
    static Scan* scan = new Scan;
    return *scan;
}

// XN_LIVE_CONTENT_INSTALLED, which PlatformMgr::Poll turns into a ContentInstalledMsg
constexpr uint32_t kXnLiveContentInstalled = 0x02000007;
// band3 is closing: the kernel may be going, so nothing more is announced
std::atomic<bool> g_stopping{false};

// tells the game of packages it hasn't listed, as the console told it of
// content it had installed: PlatformMgr passes it on as a ContentInstalledMsg,
// which marks XboxContentMgr's content changed, so its next refresh lists it
// again. Not with the scan's lock held, which the game's listings take inside
// a kernel call.
void Announce() {
    if (g_stopping) return;
    if (auto* kernel = REX_KERNEL_STATE()) kernel->BroadcastNotification(kXnLiveContentInstalled, 0);
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

std::string Upper(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    });
    return out;
}

void Finish(std::vector<Package> packages, std::vector<DroppedPackage> dropped) {
    auto& scan = TheScan();
    bool late;
    {
        std::lock_guard lock(scan.mutex);
        scan.dropped = std::move(dropped);
        scan.packages.assign(std::make_move_iterator(packages.begin()),
                             std::make_move_iterator(packages.end()));
        for (const auto& package : scan.packages) {
            scan.by_id[package.header.content_id] = &package;
            scan.paths.insert(package.path);
        }
        scan.done = true;
        late = scan.late && !scan.packages.empty();
    }
    scan.cv.notify_all();
    // a listing gave the game none while the scan ran: it lists them again now
    if (late) Announce();
}

// the setting's folders, and their names for the log
std::vector<std::filesystem::path> Folders(const std::string& setting, std::string& names) {
    auto folders = ContentFolders(setting);
    for (const auto& folder : folders) {
        if (!names.empty()) names += " | ";
        names += rex::path_to_utf8(folder);
    }
    return folders;
}

void RunScan(std::string setting) {
    const auto start = std::chrono::steady_clock::now();
    std::string names;
    auto folders = Folders(setting, names);

    // no songs folder is how band3 ships, so that alone, for an unchanged default, stays quiet
    std::error_code ec;
    if (setting == "songs" && folders.size() == 1 && !std::filesystem::is_directory(folders[0], ec)) {
        REXLOG_DEBUG("content: no folder {}", names);
        folders.clear();
    }

    std::vector<std::string> problems;
    std::vector<DroppedPackage> dropped;
    auto packages = ScanFolders(folders, kRb3TitleIds, &problems, &dropped);
    for (const auto& problem : problems) REXLOG_WARN("content: {}", problem);
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    REXLOG_INFO("content: {} packages from {} in {} ms", packages.size(),
                names.empty() ? "no folders" : names, ms);
    if (!dropped.empty()) REXLOG_INFO("content: {} copies of packages left out", dropped.size());
    Finish(std::move(packages), std::move(dropped));
}

// band3's mount of root_name, if any; g_mutex held
bool UnmountLocked(std::string_view root_name) {
    auto it = g_mounts.find(Lower(root_name));
    if (it == g_mounts.end()) return false;
    // files the game still has open on the package go first, as the SDK's own
    // close does: one left open points into the device about to be freed, and the
    // SDK's next close walks every open file (std::bad_alloc from a freed path)
    REX_KERNEL_STATE()->content_manager()->CloseOpenedFilesFromContent(root_name);
    g_vfs->UnregisterSymbolicLink(std::string(root_name) + ":");
    g_vfs->UnregisterDevice(it->second);
    g_mounts.erase(it);
    return true;
}

}  // namespace

std::vector<std::filesystem::path> ContentFolders(std::string_view setting) {
    const auto anchor = IniAnchor();
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : paths::SplitList(setting)) {
        folders.push_back(paths::Resolve(entry, anchor));
    }
    return folders;
}

void StartLiveContent(rex::filesystem::VirtualFileSystem* vfs) {
    g_vfs = vfs;
    // an unreachable network share can take tens of seconds to give up on
    std::thread(RunScan, REXCVAR_GET(content_folders)).detach();
}

std::vector<Package> LivePackages() {
    // a big library on a slow network folder can take longer than this, and the
    // game waits on each listing
    static const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    auto& scan = TheScan();
    std::unique_lock lock(scan.mutex);
    if (scan.cv.wait_until(lock, deadline, [&] { return scan.done; })) {
        return std::vector<Package>(scan.packages.begin(), scan.packages.end());
    }
    if (!scan.late) {
        scan.late = true;
        REXLOG_WARN("content: scanning the content folders took longer than 60 s, so their "
                    "packages aren't listed until it's done");
    }
    return {};
}

std::vector<DroppedPackage> DroppedPackages() {
    auto& scan = TheScan();
    std::lock_guard lock(scan.mutex);
    return scan.dropped;
}

const Package* FindLivePackage(std::string_view file_name) {
    auto& scan = TheScan();
    std::lock_guard lock(scan.mutex);
    if (!scan.done) return nullptr;
    auto it = scan.by_id.find(Upper(file_name));
    return it == scan.by_id.end() ? nullptr : it->second;
}

size_t AddLivePackages(const std::vector<std::filesystem::path>& files) {
    auto& scan = TheScan();
    {
        // the first scan may not have read these yet, or may have missed them
        std::unique_lock lock(scan.mutex);
        scan.cv.wait(lock, [&] { return scan.done || g_stopping; });
        if (g_stopping) return 0;
    }
    std::vector<Package> found;
    for (const auto& file : files) {
        {
            std::lock_guard lock(scan.mutex);
            if (scan.paths.contains(file)) continue;
        }
        if (auto package = ReadPackage(file, kRb3TitleIds)) found.push_back(std::move(*package));
    }
    size_t added = 0;
    {
        std::lock_guard lock(scan.mutex);
        for (auto& package : found) {
            scan.paths.insert(package.path);
            // another file of a package already listed (a copy, or a newer
            // version): the game reads the one it has until the next launch
            if (const auto it = scan.by_id.find(package.header.content_id); it != scan.by_id.end()) {
                scan.dropped.push_back({package.path, it->second->path});
                continue;
            }
            REXLOG_INFO("content: found {} ({})", rex::path_to_utf8(package.path),
                        package.header.content_id);
            scan.packages.push_back(std::move(package));
            scan.by_id[scan.packages.back().header.content_id] = &scan.packages.back();
            added++;
        }
    }
    if (added) Announce();
    return added;
}

void StopLiveContent() {
    g_stopping = true;
    TheScan().cv.notify_all();
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
