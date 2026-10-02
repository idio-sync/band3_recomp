#include "live_content.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <map>
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
    // rescans go one at a time
    std::mutex rescan_mutex;
};

Scan& TheScan() {
    static Scan* scan = new Scan;
    return *scan;
}

// XN_LIVE_CONTENT_INSTALLED, which PlatformMgr::Poll turns into a ContentInstalledMsg
constexpr uint32_t kXnLiveContentInstalled = 0x02000007;

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

void Finish(std::vector<Package> packages) {
    auto& scan = TheScan();
    {
        std::lock_guard lock(scan.mutex);
        scan.packages.assign(std::make_move_iterator(packages.begin()),
                             std::make_move_iterator(packages.end()));
        for (const auto& package : scan.packages) scan.by_id[package.header.content_id] = &package;
        scan.done = true;
    }
    scan.cv.notify_all();
}

#ifdef _WIN32
// the setting's folders, and their names for the log
std::vector<std::filesystem::path> Folders(const std::string& setting, std::string& names) {
    const auto anchor = IniAnchor();
    std::vector<std::filesystem::path> folders;
    for (const auto& entry : paths::SplitList(setting)) {
        folders.push_back(paths::Resolve(entry, anchor));
        if (!names.empty()) names += " | ";
        names += rex::path_to_utf8(folders.back());
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
    auto packages = ScanFolders(folders, kRb3TitleIds, &problems);
    for (const auto& problem : problems) REXLOG_WARN("content: {}", problem);
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
    // listing packages the game then couldn't close would do no good: they open
    // through the XContentCrossTitleCreate override, but the XamContentClose
    // override that closes them needs the SDK's own export, found only on Windows
    REXLOG_INFO("content: content folders need Windows for now, not reading {}",
                REXCVAR_GET(content_folders));
    Finish({});
#endif
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

const Package* FindLivePackage(std::string_view file_name) {
    auto& scan = TheScan();
    std::lock_guard lock(scan.mutex);
    if (!scan.done) return nullptr;
    auto it = scan.by_id.find(Upper(file_name));
    return it == scan.by_id.end() ? nullptr : it->second;
}

size_t RescanLiveContent() {
#ifdef _WIN32
    auto& scan = TheScan();
    std::lock_guard rescan(scan.rescan_mutex);
    {
        // before the first scan is done, it finds them anyway
        std::lock_guard lock(scan.mutex);
        if (!scan.done) return 0;
    }
    std::string names;
    std::vector<std::string> problems;
    auto found = ScanFolders(Folders(REXCVAR_GET(content_folders), names), kRb3TitleIds, &problems);
    size_t added = 0;
    {
        std::lock_guard lock(scan.mutex);
        for (auto& package : found) {
            if (scan.by_id.contains(package.header.content_id)) continue;
            REXLOG_INFO("content: found {} ({})", rex::path_to_utf8(package.path),
                        package.header.content_id);
            scan.packages.push_back(std::move(package));
            scan.by_id[scan.packages.back().header.content_id] = &scan.packages.back();
            added++;
        }
    }
    if (added) {
        // as the console told the game of content it had installed: PlatformMgr
        // passes it on as a ContentInstalledMsg, which marks XboxContentMgr's
        // content changed, so its next StartRefresh lists it again. Outside the
        // lock, which the game's listings take inside a kernel call.
        REX_KERNEL_STATE()->BroadcastNotification(kXnLiveContentInstalled, 0);
    }
    return added;
#else
    return 0;
#endif
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
