#include "src/Content/ark_archive.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>

namespace band3::content {

namespace {

// the header's own version: Rock Band 3's
constexpr int32_t kArkVersion = 6;
// bigger than any header (Rock Band 3's is half a megabyte)
constexpr uintmax_t kMaxHeader = 64u << 20;

std::string Lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = char(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

// division rounding down, as the generator's (written with Python's //)
int64_t FloorDiv(int64_t a, int64_t b) { return a >= 0 ? a / b : -((-a + b - 1) / b); }

int64_t NextKey(int64_t key) {
    const int64_t q = FloorDiv(key, 0x1F31D);
    key = (key - q * 0x1F31D) * 0x41A7 - q * 0xB14;
    return key <= 0 ? key + 0x7FFFFFFF : key;
}

// reads the deciphered header from the front, failing (and staying failed)
// past its end
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}

    bool ok() const { return ok_; }

    int32_t I32() { return int32_t(U32()); }

    uint32_t U32() {
        const std::span<const uint8_t> b = Take(4);
        if (b.empty()) return 0;
        return uint32_t(b[0]) | uint32_t(b[1]) << 8 | uint32_t(b[2]) << 16 | uint32_t(b[3]) << 24;
    }

    uint64_t U64() {
        const uint64_t lo = U32();
        return lo | uint64_t(U32()) << 32;
    }

    // a count of what follows: none if negative or past what's left
    size_t Count(size_t each) {
        const int32_t n = I32();
        if (n < 0 || size_t(n) * each > data_.size() - at_) {
            ok_ = false;
            return 0;
        }
        return size_t(n);
    }

    std::span<const uint8_t> Take(size_t n) {
        if (!ok_ || n > data_.size() - at_) {
            ok_ = false;
            return {};
        }
        const std::span<const uint8_t> out = data_.subspan(at_, n);
        at_ += n;
        return out;
    }

private:
    std::span<const uint8_t> data_;
    size_t at_ = 0;
    bool ok_ = true;
};

}

std::vector<uint8_t> DecryptDtb(std::span<const uint8_t> data) {
    if (data.size() < 4) return {};
    int64_t key = int32_t(uint32_t(data[0]) | uint32_t(data[1]) << 8 | uint32_t(data[2]) << 16 |
                          uint32_t(data[3]) << 24);
    std::vector<uint8_t> out(data.size() - 4);
    for (size_t i = 0; i < out.size(); i++) {
        key = NextKey(key);
        out[i] = data[i + 4] ^ uint8_t(key & 0xFF);
    }
    return out;
}

std::optional<ArkArchive> ArkArchive::Open(const std::filesystem::path& header, std::string& error) {
    std::error_code ec;
    const uintmax_t size = std::filesystem::file_size(header, ec);
    if (ec || size < 4 || size > kMaxHeader) {
        error = "its header can't be read";
        return std::nullopt;
    }
    std::vector<uint8_t> raw(size_t(size), 0);
    std::ifstream in(header, std::ios::binary);
    if (!in.read(reinterpret_cast<char*>(raw.data()), std::streamsize(raw.size()))) {
        error = "its header can't be read";
        return std::nullopt;
    }
    const std::vector<uint8_t> h = DecryptDtb(raw);
    Reader r(h);
    const int32_t version = r.I32();
    if (version != kArkVersion) {
        error = std::format("its header is version {}; band3 reads only 6", version);
        return std::nullopt;
    }
    ArkArchive ark;
    ark.folder_ = header.parent_path();
    r.I32();      // unknown, 1
    r.Take(16);   // a hash
    r.I32();      // the part count, again below
    for (size_t i = 0, n = r.Count(4); i < n; i++) ark.part_sizes_.push_back(r.U32());
    for (size_t i = 0, n = r.Count(4); i < n && r.ok(); i++) {
        const std::span<const uint8_t> name = r.Take(r.Count(1));
        // "gen/main_xbox_0.ark": the part beside the header
        const std::string path(name.begin(), name.end());
        ark.part_names_.push_back(path.substr(path.find_last_of("/\\") + 1));
    }
    r.Take(r.Count(4) * 4);  // the parts' checksums
    const std::span<const uint8_t> blob = r.Take(r.Count(1));
    std::vector<uint32_t> offsets;
    for (size_t i = 0, n = r.Count(4); i < n; i++) offsets.push_back(r.U32());
    auto text = [&](int32_t index, std::string& out) {
        if (index < 0 || size_t(index) >= offsets.size() || offsets[index] >= blob.size()) return false;
        const auto start = blob.begin() + offsets[index];
        out.assign(start, std::find(start, blob.end(), uint8_t(0)));
        return true;
    };
    for (size_t i = 0, n = r.Count(24); i < n && r.ok(); i++) {
        const uint64_t offset = r.U64();
        const int32_t name = r.I32(), folder = r.I32();
        const uint32_t file_size = r.U32();
        r.U32();  // its size inflated
        std::string file, dir;
        if (!text(name, file) || !text(folder, dir)) continue;
        ark.files_.emplace(Lower(dir + "/" + file), Entry{offset, file_size});
    }
    if (!r.ok() || ark.part_sizes_.empty() || ark.part_sizes_.size() != ark.part_names_.size()) {
        error = "its header is cut short";
        return std::nullopt;
    }
    return ark;
}

bool ArkArchive::Has(std::string_view name) const { return files_.contains(Lower(name)); }

bool ArkArchive::Read(std::string_view name, std::vector<uint8_t>& out, std::string& error) const {
    out.clear();
    const auto it = files_.find(Lower(name));
    if (it == files_.end()) {
        error = "the archive has no " + std::string(name);
        return false;
    }
    uint64_t offset = it->second.offset;
    out.resize(it->second.size);
    size_t done = 0;
    // a file in one part, or (if the archive splits one) across the next
    for (size_t part = 0; part < part_sizes_.size() && done < out.size(); part++) {
        if (offset >= part_sizes_[part]) {
            offset -= part_sizes_[part];
            continue;
        }
        const size_t n = size_t(std::min<uint64_t>(part_sizes_[part] - offset, out.size() - done));
        std::ifstream in(folder_ / part_names_[part], std::ios::binary);
        if (!in.seekg(std::streamoff(offset)) ||
            !in.read(reinterpret_cast<char*>(out.data() + done), std::streamsize(n))) {
            error = "the archive's " + part_names_[part] + " can't be read";
            out.clear();
            return false;
        }
        done += n;
        offset = 0;
    }
    if (done < out.size()) {
        error = std::string(name) + " runs past the archive's end";
        out.clear();
        return false;
    }
    return true;
}

}
