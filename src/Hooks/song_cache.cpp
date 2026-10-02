#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>
#include <rex/types.h>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>
#include "src/Game/SongCache.h"

// Which song cache the game mounted, from CacheMgrXbox::MountAsync, as
// RB3Enhanced finds it (source/xbox360_content.c), and deleting it.

extern "C" void __imp__CacheMgrXbox__MountAsync(PPCContext& ctx, uint8_t* base);

namespace band3::song_cache {

namespace {

// CacheIDXbox: {vtable, String mStrCacheName, XCONTENT_DATA mContentData}
constexpr uint32_t kCacheID_ContentData = 0x10;
// XCONTENT_DATA: {device_id, content_type, display_name[128] (UTF-16), file_name[42]}
constexpr uint32_t kContentData_Type = 0x4;
constexpr uint32_t kContentData_FileName = 0x108;
constexpr size_t kFileNameLength = 42;

// what the user data root keeps marked for deletion, a path relative to it
// per line
constexpr const char* kPendingFile = "pending_deletes.txt";

std::mutex g_mutex;
std::string g_file_name;  // the mounted song cache's, or empty
uint32_t g_content_type = 0;

bool IsSongCache(const std::string& name) { return name == "songcache" || name == "rbdxcache"; }

std::string Hex(uint32_t value) {
    char text[9];
    std::snprintf(text, sizeof(text), "%08X", value);
    return text;
}

}

bool RequestDelete() {
    std::string file_name;
    uint32_t content_type;
    {
        std::lock_guard lock(g_mutex);
        file_name = g_file_name;
        content_type = g_content_type;
    }
    if (file_name.empty()) {
        REXLOG_WARN("Song cache: the game hasn't mounted one, so there's none to delete");
        return false;
    }
    auto* runtime = rex::Runtime::instance();
    auto* kernel = rex::system::kernel_state();
    if (!runtime || !kernel) return false;

    // <user data root>/<profile XUID>/<title ID>/<content type>/<file name>, and
    // its header in .../<title ID>/Headers/<content type>/<file name>.header,
    // as the SDK keeps content; whichever profile folders have it
    const std::filesystem::path& root = runtime->user_data_root();
    const std::string title = Hex(kernel->title_id());
    const std::string type = Hex(content_type);
    std::vector<std::filesystem::path> found;
    std::error_code ec;
    for (const auto& profile : std::filesystem::directory_iterator(root, ec)) {
        if (!profile.is_directory(ec)) continue;
        const std::filesystem::path title_dir = profile.path() / title;
        const std::filesystem::path package = title_dir / type / file_name;
        if (!std::filesystem::is_directory(package, ec)) continue;
        found.push_back(std::filesystem::relative(package, root, ec));
        const std::filesystem::path header =
            title_dir / "Headers" / type / (file_name + ".header");
        if (std::filesystem::exists(header, ec)) {
            found.push_back(std::filesystem::relative(header, root, ec));
        }
    }
    if (found.empty()) {
        REXLOG_WARN("Song cache: couldn't find {} under {}", file_name, root.string());
        return false;
    }

    std::ofstream pending(root / kPendingFile, std::ios::app);
    for (const auto& path : found) pending << path.generic_string() << '\n';
    if (!pending) {
        REXLOG_WARN("Song cache: couldn't write {}", (root / kPendingFile).string());
        return false;
    }
    REXLOG_INFO("Song cache: {} is deleted when band3 next starts", file_name);
    return true;
}

void DeletePending(const std::filesystem::path& user_data_root) {
    const std::filesystem::path list = user_data_root / kPendingFile;
    std::ifstream pending(list);
    if (!pending) return;
    for (std::string line; std::getline(pending, line);) {
        const std::filesystem::path path(line);
        // only what RequestDelete wrote: paths inside the user data root
        bool inside = !line.empty() && path.is_relative();
        for (const auto& part : path) inside = inside && part != "..";
        if (!inside) continue;
        std::error_code ec;
        const auto removed = std::filesystem::remove_all(user_data_root / path, ec);
        if (ec) {
            REXLOG_WARN("Song cache: couldn't delete {}: {}", line, ec.message());
        } else if (removed) {
            REXLOG_INFO("Song cache: deleted {}", line);
        }
    }
    pending.close();
    std::error_code ec;
    std::filesystem::remove(list, ec);
}

}

// CacheMgrXbox::MountAsync(CacheMgrXbox*, CacheIDXbox*, Cache**, void*)
extern "C" REX_FUNC(CacheMgrXbox__MountAsync)
{
    const uint32_t cache_id = ctx.r4.u32;
    __imp__CacheMgrXbox__MountAsync(ctx, base);
    if (!cache_id) return;

    using namespace band3::song_cache;
    const uint32_t data = cache_id + kCacheID_ContentData;
    const char* raw = rex::memory::GuestPtr<const char*>(base, data + kContentData_FileName);
    const std::string file_name(raw, strnlen(raw, kFileNameLength));
    if (!IsSongCache(file_name)) return;
    std::lock_guard lock(g_mutex);
    g_file_name = file_name;
    g_content_type = *rex::memory::GuestPtr<rex::be<uint32_t>*>(base, data + kContentData_Type);
    REXLOG_INFO("Song cache: the game mounted {}", file_name);
}
