#include "package_scan.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <system_error>
#include <thread>

namespace band3::content {

namespace {

namespace fs = std::filesystem;

uint32_t U32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

constexpr uint32_t kCon = 0x434F4E20, kLive = 0x4C495645, kPirs = 0x50495253;

std::string Utf8(const fs::path& path) {
    auto s = path.u8string();
    return std::string(s.begin(), s.end());
}

struct FolderScan {
    std::vector<Package> packages;
    std::vector<std::string> problems;
};

// one folder and its subfolders, listed a folder at a time: recursive_directory_iterator
// ends at its first error, which would lose the rest of the folder
FolderScan ScanFolder(const fs::path& folder, std::span<const uint32_t> title_ids) {
    FolderScan out;
    std::error_code ec;
    if (!fs::is_directory(folder, ec)) {
        out.problems.push_back("no folder " + Utf8(folder));
        return out;
    }
    std::vector<fs::directory_iterator> listing;  // the folders being listed, innermost last
    std::vector<fs::path> pending;  // updates to put in place
    std::vector<fs::path> set_aside;  // requests to set packages aside
    auto enter = [&](const fs::path& dir) {
        std::error_code error;
        fs::directory_iterator it(dir, fs::directory_options::skip_permission_denied, error);
        if (error) {
            out.problems.push_back(Utf8(dir) + ": " + error.message());
        } else {
            listing.push_back(std::move(it));
        }
    };
    enter(folder);
    while (!listing.empty()) {
        auto& it = listing.back();
        if (it == fs::directory_iterator()) {
            listing.pop_back();
            continue;
        }
        const fs::directory_entry entry = *it;
        it.increment(ec);
        if (ec) {
            // the rest of this folder can't be listed; an iterator that failed
            // may not have moved, so it's dropped rather than tried again
            out.problems.push_back(Utf8(entry.path().parent_path()) + ": " + ec.message());
            listing.pop_back();
        }
        // a symlink or junction isn't followed into, as recursive_directory_iterator doesn't
        if (entry.symlink_status(ec).type() == fs::file_type::directory) {
            enter(entry.path());
            continue;
        }
        if (!entry.is_regular_file(ec)) continue;
        if (entry.path().extension() == fs::path(kPendingSuffix)) {
            pending.push_back(entry.path());
            continue;
        }
        if (entry.path().extension() == fs::path(kSetAsideRequestSuffix)) {
            set_aside.push_back(entry.path());
            continue;
        }
        if (auto package = ReadPackage(entry.path(), title_ids)) out.packages.push_back(std::move(*package));
    }
    // after the walk, which may have read the old file already; one at a time,
    // since folders that overlap ("songs" and "songs/rhythmverse") meet the same
    // update on two threads
    static std::mutex updates_mutex;
    for (const fs::path& update : pending) {
        {
            std::lock_guard lock(updates_mutex);
            std::error_code exists_ec;
            // another thread has put it in place: only the reading is left
            if (fs::exists(update, exists_ec)) {
                if (std::string problem = ApplyPendingUpdate(update); !problem.empty()) {
                    out.problems.push_back(problem);
                    continue;
                }
            }
        }
        const fs::path target = fs::path(update).replace_extension();
        std::erase_if(out.packages, [&](const Package& p) { return p.path == target; });
        if (auto package = ReadPackage(target, title_ids)) out.packages.push_back(std::move(*package));
    }
    // then the packages asked to be set aside, which may have been read too
    for (const fs::path& request : set_aside) {
        std::lock_guard lock(updates_mutex);
        std::error_code exists_ec;
        if (!fs::exists(request, exists_ec)) continue;  // done by another thread
        if (std::string problem = ApplySetAside(request); !problem.empty()) out.problems.push_back(problem);
        const fs::path target = fs::path(request).replace_extension();
        if (!fs::exists(target, exists_ec)) {
            std::erase_if(out.packages, [&](const Package& p) { return p.path == target; });
        }
    }
    return out;
}

}

std::optional<PackageHeader> ParsePackageHeader(std::span<const uint8_t> b) {
    if (b.size() < kHeaderBytes) return std::nullopt;
    PackageHeader h;
    h.magic = U32(b, 0);
    if (h.magic != kCon && h.magic != kLive && h.magic != kPirs) return std::nullopt;
    for (size_t i = 0; i < 16; i++) {
        const size_t at = 0x22C + i * 0x10;
        if (U32(b, at + 12)) h.license_mask |= U32(b, at + 8);
    }
    static constexpr char kHex[] = "0123456789ABCDEF";
    for (size_t i = 0; i < 20; i++) {
        h.content_id += kHex[b[0x32C + i] >> 4];
        h.content_id += kHex[b[0x32C + i] & 0xF];
    }
    h.content_type = U32(b, 0x344);
    h.title_id = U32(b, 0x360);
    for (size_t i = 0; i < 128; i++) {
        char16_t c = char16_t(b[0x411 + i * 2] << 8 | b[0x411 + i * 2 + 1]);
        if (!c) break;
        h.display_name += c;
    }
    return h;
}

bool IsSetAside(const fs::path& path) {
    const fs::path ext = path.extension();
    return ext == fs::path(kPartialSuffix) || ext == fs::path(kPendingSuffix) ||
           ext == fs::path(kReplacedSuffix) || ext == fs::path(kSetAsideSuffix) ||
           ext == fs::path(kSetAsideRequestSuffix);
}

std::string RequestSetAside(const fs::path& file) {
    std::error_code ec;
    if (!fs::is_regular_file(file, ec)) return Utf8(file) + " isn't there";
    fs::path done = file, request = file;
    done += kSetAsideSuffix;
    request += kSetAsideRequestSuffix;
    if (fs::exists(done, ec)) return Utf8(done) + " is there already: put it back or move it first";
    std::ofstream marker(request, std::ios::binary | std::ios::trunc);
    if (!marker) return "couldn't write " + Utf8(request);
    return {};
}

std::string ApplySetAside(const fs::path& request) {
    const fs::path file = fs::path(request).replace_extension();
    fs::path done = file;
    done += kSetAsideSuffix;
    std::error_code ec;
    std::string problem;
    if (!fs::exists(file, ec)) {
        problem = Utf8(file) + ": gone before it could be set aside";
    } else if (fs::exists(done, ec)) {
        problem = Utf8(file) + ": not set aside, as " + Utf8(done) + " is there already";
    } else {
        fs::rename(file, done, ec);
        if (ec) return Utf8(file) + ": couldn't set it aside: " + ec.message();  // asked again next time
    }
    fs::remove(request, ec);
    return problem;
}

std::vector<SetAsideFile> FindSetAside(const std::vector<fs::path>& folders) {
    std::vector<SetAsideFile> found;
    for (const fs::path& folder : folders) {
        std::error_code ec;
        fs::recursive_directory_iterator it(folder, fs::directory_options::skip_permission_denied, ec);
        for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
            std::error_code file_ec;
            if (!it->is_regular_file(file_ec)) continue;
            const fs::path ext = it->path().extension();
            const bool request = ext == fs::path(kSetAsideRequestSuffix);
            if (request || ext == fs::path(kSetAsideSuffix)) {
                found.push_back({fs::path(it->path()).replace_extension(), request});
            }
        }
    }
    std::ranges::sort(found, {}, &SetAsideFile::file);
    // folders that overlap meet the same file twice
    const auto same = [](const SetAsideFile& a, const SetAsideFile& b) {
        return a.file == b.file && a.next_launch == b.next_launch;
    };
    found.erase(std::unique(found.begin(), found.end(), same), found.end());
    return found;
}

std::string PutBack(const SetAsideFile& set_aside) {
    std::error_code ec;
    fs::path from = set_aside.file;
    if (set_aside.next_launch) {
        from += kSetAsideRequestSuffix;
        fs::remove(from, ec);  // gone already is as good
        if (ec) return "couldn't take back " + Utf8(from) + ": " + ec.message();
        return {};
    }
    from += kSetAsideSuffix;
    if (fs::exists(set_aside.file, ec)) {
        return Utf8(set_aside.file) + " is there again: move it before putting this one back";
    }
    fs::rename(from, set_aside.file, ec);
    if (ec) return "couldn't put back " + Utf8(set_aside.file) + ": " + ec.message();
    return {};
}

std::string ApplyPendingUpdate(const fs::path& pending) {
    const fs::path target = fs::path(pending).replace_extension();
    std::error_code ec;
    if (fs::exists(target, ec)) {
        fs::path kept = target;
        kept += kReplacedSuffix;
        for (int n = 2; fs::exists(kept, ec); n++) {
            kept = target;
            kept += "." + std::to_string(n) + std::string(kReplacedSuffix);
        }
        fs::rename(target, kept, ec);
        if (ec) return Utf8(target) + ": couldn't set it aside for its update: " + ec.message();
        fs::rename(pending, target, ec);
        if (ec) {
            // the old one goes back, so the song is still there
            std::error_code back;
            fs::rename(kept, target, back);
            return Utf8(pending) + ": couldn't put the update in place: " + ec.message();
        }
        return {};
    }
    fs::rename(pending, target, ec);
    if (ec) return Utf8(pending) + ": couldn't put the update in place: " + ec.message();
    return {};
}

std::optional<Package> ReadPackage(const fs::path& path, std::span<const uint32_t> title_ids) {
    if (IsSetAside(path)) return std::nullopt;
    std::vector<uint8_t> bytes(kHeaderBytes);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return std::nullopt;
    auto header = ParsePackageHeader(bytes);
    if (!header || std::ranges::find(title_ids, header->title_id) == title_ids.end()) return std::nullopt;
    return Package{path, std::move(*header)};
}

std::vector<Package> ScanFolders(const std::vector<fs::path>& folders,
                                 std::span<const uint32_t> title_ids, std::vector<std::string>* problems,
                                 std::vector<DroppedPackage>* dropped) {
    // one thread a folder, so a slow network folder doesn't hold up a local one
    std::vector<FolderScan> scans(folders.size());
    {
        std::vector<std::thread> threads;
        for (size_t i = 0; i < folders.size(); i++) {
            threads.emplace_back([&, i] { scans[i] = ScanFolder(folders[i], title_ids); });
        }
        for (auto& thread : threads) thread.join();
    }
    // combined in the folders' order, so the first found of a content ID is still the first listed
    std::vector<Package> found;
    std::map<std::string, fs::path> kept;  // content ID -> the file listed
    for (auto& scan : scans) {
        for (auto& package : scan.packages) {
            const auto [first, added] = kept.emplace(package.header.content_id, package.path);
            if (added) {
                found.push_back(std::move(package));
                continue;
            }
            std::error_code ec;
            const bool same = package.path == first->second || fs::equivalent(package.path, first->second, ec);
            if (dropped && !same) dropped->push_back({package.path, first->second});
        }
        if (problems) problems->insert(problems->end(), scan.problems.begin(), scan.problems.end());
    }
    return found;
}

}
