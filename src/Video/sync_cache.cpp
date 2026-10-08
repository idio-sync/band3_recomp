#include "src/Video/sync_cache.h"

#include <charconv>
#include <cstring>
#include <format>
#include <fstream>
#include <sstream>
#include <system_error>

#include "src/Video/sync_align.h"

namespace band3::video {

namespace {
constexpr char kMagic[8] = {'B', '3', 'E', 'N', 'V', '1', 0, 0};
}

uint64_t FileStamp(const std::filesystem::path& path) {
    std::error_code ec;
    const uint64_t size = std::filesystem::file_size(path, ec);
    if (ec) return 0;
    const auto time = std::filesystem::last_write_time(path, ec);
    if (ec) return 0;
    const uint64_t ticks = uint64_t(time.time_since_epoch().count());
    return (size * 0x9E3779B97F4A7C15ull) ^ ticks ^ 1;
}

bool SaveEnvelope(const std::filesystem::path& path, std::span<const float> frames,
                  uint64_t stamp) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    const uint32_t rate = kEnvelopeRate, count = uint32_t(frames.size());
    out.write(kMagic, sizeof(kMagic));
    out.write(reinterpret_cast<const char*>(&rate), 4);
    out.write(reinterpret_cast<const char*>(&stamp), 8);
    out.write(reinterpret_cast<const char*>(&count), 4);
    out.write(reinterpret_cast<const char*>(frames.data()), std::streamsize(frames.size() * 4));
    return bool(out);
}

std::string ResultText(const SyncResult& r) {
    return std::format("offset = {:.3f}\nscore = {:.3f}\nmargin = {:.3f}\nonset = {:.3f}\n"
                       "confident = {}\n",
                       r.offset, r.score, r.margin, r.onset_score, r.confident ? 1 : 0);
}

std::optional<SyncResult> ParseResult(std::string_view text) {
    SyncResult r;
    int fields = 0;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text.remove_prefix(end == std::string_view::npos ? text.size() : end + 1);
        const size_t eq = line.find(" = ");
        if (eq == std::string_view::npos) continue;
        const std::string_view key = line.substr(0, eq), value = line.substr(eq + 3);
        double v = 0;
        if (std::from_chars(value.data(), value.data() + value.size(), v).ec != std::errc())
            continue;
        if (key == "offset") r.offset = v, fields++;
        else if (key == "score") r.score = v, fields++;
        else if (key == "margin") r.margin = v, fields++;
        else if (key == "onset") r.onset_score = v, fields++;
        else if (key == "confident") r.confident = v != 0, fields++;
    }
    if (fields < 5) return std::nullopt;
    r.found = true;
    return r;
}

bool SaveResult(const std::filesystem::path& path, const SyncResult& r) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    out << ResultText(r);
    return bool(out);
}

std::optional<SyncResult> LoadResult(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::stringstream text;
    text << in.rdbuf();
    return ParseResult(text.str());
}

std::optional<std::vector<float>> LoadEnvelope(const std::filesystem::path& path,
                                               uint64_t stamp) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    char magic[8];
    uint32_t rate = 0, count = 0;
    uint64_t saved = 0;
    in.read(magic, 8);
    in.read(reinterpret_cast<char*>(&rate), 4);
    in.read(reinterpret_cast<char*>(&saved), 8);
    in.read(reinterpret_cast<char*>(&count), 4);
    if (!in || std::memcmp(magic, kMagic, 8) != 0 || rate != kEnvelopeRate) return std::nullopt;
    if (stamp && saved != stamp) return std::nullopt;
    if (count > 200u * 60 * 60) return std::nullopt;  // an hour, at most
    std::vector<float> frames(count);
    in.read(reinterpret_cast<char*>(frames.data()), std::streamsize(count) * 4);
    if (!in) return std::nullopt;
    return frames;
}

}
