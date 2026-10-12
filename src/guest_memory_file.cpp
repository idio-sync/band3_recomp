#include "guest_memory_file.h"

#include <rex/logging.h>

#ifdef __linux__
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#endif

namespace band3::guest_memory_file {

#ifdef __linux__
namespace {

std::string ReadAll(const std::string& path) {
    std::ifstream f(path);
    std::ostringstream s;
    s << f.rdbuf();
    return s.str();
}

}

void Release() {
    namespace fs = std::filesystem;
    const std::set<std::string> own = MappedGuestMemoryFiles(ReadAll("/proc/self/maps"));
    if (own.empty()) {
        REXLOG_WARN("guest memory: no {}{}* file mapped, so none to unlink (has the SDK moved "
                    "it?)",
                    kShmDir, kPrefix);
    }
    // this user's guest memory files
    std::vector<std::string> files;
    std::error_code ec;
    for (const fs::directory_entry& e : fs::directory_iterator(std::string(kShmDir), ec)) {
        if (!IsGuestMemoryName(e.path().filename().string())) continue;
        struct stat st{};
        if (::stat(e.path().c_str(), &st) != 0 || st.st_uid != ::getuid()) continue;
        files.push_back(e.path().string());
    }
    // what every process this user can read maps (another user's can't be
    // read, and has no business with this user's files)
    std::set<std::string> mapped;
    if (files.size() > own.size()) {
        for (const fs::directory_entry& e : fs::directory_iterator("/proc", ec)) {
            const std::string pid = e.path().filename().string();
            if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos) continue;
            mapped.merge(MappedGuestMemoryFiles(ReadAll(e.path().string() + "/maps")));
        }
    }
    mapped.insert(own.begin(), own.end());
    uint64_t left_bytes = 0;
    int left = 0;
    for (const std::string& f : FilesToRemove(files, own, mapped)) {
        struct stat st{};
        // the pages a file holds, not its (sparse) length
        const uint64_t bytes = ::stat(f.c_str(), &st) == 0 ? uint64_t(st.st_blocks) * 512 : 0;
        if (::unlink(f.c_str()) != 0) {
            REXLOG_WARN("guest memory: couldn't unlink {}: {}", f, std::strerror(errno));
            continue;
        }
        if (own.contains(f)) {
            REXLOG_INFO("guest memory: unlinked {}, so it goes when band3 does, however it ends",
                        f);
        } else {
            left++;
            left_bytes += bytes;
        }
    }
    if (left) {
        REXLOG_WARN("guest memory: removed {} file(s) earlier runs left in {}, freeing {} MB of "
                    "RAM",
                    left, kShmDir, left_bytes >> 20);
    }
}
#else
void Release() {}
#endif

}  // namespace band3::guest_memory_file
