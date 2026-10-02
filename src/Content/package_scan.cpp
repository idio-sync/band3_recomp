#include "package_scan.h"
#include <algorithm>
#include <fstream>
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
        if (auto package = ReadPackage(entry.path(), title_ids)) out.packages.push_back(std::move(*package));
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

std::optional<Package> ReadPackage(const fs::path& path, std::span<const uint32_t> title_ids) {
    if (path.extension() == fs::path(kPartialSuffix)) return std::nullopt;
    std::vector<uint8_t> bytes(kHeaderBytes);
    std::ifstream file(path, std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return std::nullopt;
    auto header = ParsePackageHeader(bytes);
    if (!header || std::ranges::find(title_ids, header->title_id) == title_ids.end()) return std::nullopt;
    return Package{path, std::move(*header)};
}

std::vector<Package> ScanFolders(const std::vector<fs::path>& folders,
                                 std::span<const uint32_t> title_ids, std::vector<std::string>* problems) {
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
    std::set<std::string> seen;
    for (auto& scan : scans) {
        for (auto& package : scan.packages) {
            if (seen.insert(package.header.content_id).second) found.push_back(std::move(package));
        }
        if (problems) problems->insert(problems->end(), scan.problems.begin(), scan.problems.end());
    }
    return found;
}

}
