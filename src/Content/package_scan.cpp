#include "package_scan.h"
#include <fstream>
#include <set>
#include <system_error>

namespace band3::content {

namespace {

uint32_t U32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) << 24 | uint32_t(b[at + 1]) << 16 | uint32_t(b[at + 2]) << 8 | b[at + 3];
}

constexpr uint32_t kCon = 0x434F4E20, kLive = 0x4C495645, kPirs = 0x50495253;

std::string Utf8(const std::filesystem::path& path) {
    auto s = path.u8string();
    return std::string(s.begin(), s.end());
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

std::vector<Package> ScanFolders(const std::vector<std::filesystem::path>& folders,
                                 uint32_t title_id, std::vector<std::string>* problems) {
    std::vector<Package> found;
    std::set<std::string> seen;
    for (const auto& folder : folders) {
        std::error_code ec;
        if (!std::filesystem::is_directory(folder, ec)) {
            if (problems) problems->push_back("no folder " + Utf8(folder));
            continue;
        }
        auto it = std::filesystem::recursive_directory_iterator(
            folder, std::filesystem::directory_options::skip_permission_denied, ec);
        for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            std::vector<uint8_t> bytes(kHeaderBytes);
            std::ifstream file(it->path(), std::ios::binary);
            if (!file.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) continue;
            auto header = ParsePackageHeader(bytes);
            if (!header || header->title_id != title_id) continue;
            if (!seen.insert(header->content_id).second) continue;
            found.push_back({it->path(), std::move(*header)});
        }
        if (ec && problems) problems->push_back(Utf8(folder) + ": " + ec.message());
    }
    return found;
}

}
