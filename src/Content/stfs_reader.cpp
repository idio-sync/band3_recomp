#include "stfs_reader.h"
#include <algorithm>
#ifdef _WIN32
#include <share.h>
#endif

namespace band3::content {

namespace {

// how many blocks one hash table covers at levels 0, 1 and 2
constexpr uint32_t kPerLevel[3] = {170, 170 * 170, 170 * 170 * 170};
constexpr uint32_t kEndOfChain = 0xFFFFFF;
constexpr size_t kEntrySize = 0x40;      // a file table entry
constexpr size_t kHashEntrySize = 0x18;  // a hash table entry: SHA-1, then its info
constexpr uint32_t kActiveIndex = 0x40000000;  // a level 1/2 entry: its table's second copy is in use

// the header, as far as the volume type (the metadata starts at 0x344)
constexpr size_t kHeaderSize = 0x340;
constexpr size_t kDescriptor = 0x379;
constexpr size_t kVolumeType = 0x3A9;
constexpr size_t kHeaderBytes = 0x3AD;

constexpr uint32_t kCon = 0x434F4E20, kLive = 0x4C495645, kPirs = 0x50495253;

uint32_t Be32(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
uint16_t Be16(const uint8_t* p) { return uint16_t(p[0] << 8 | p[1]); }
uint32_t Le24(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16; }

int Seek(std::FILE* file, uint64_t offset) {
#ifdef _WIN32
    return _fseeki64(file, int64_t(offset), SEEK_SET);
#else
    return fseeko(file, off_t(offset), SEEK_SET);
#endif
}

}

StfsReader::~StfsReader() {
    if (file_) std::fclose(file_);
}

std::unique_ptr<StfsReader> StfsReader::Open(const std::filesystem::path& path, std::string* error) {
    auto fail = [&](const char* why) -> std::unique_ptr<StfsReader> {
        if (error) *error = why;
        return nullptr;
    };
    std::unique_ptr<StfsReader> reader(new StfsReader);
#ifdef _WIN32
    // shared as the SDK's fopen shares it: nothing denied to others
    reader->file_ = _wfsopen(path.c_str(), L"rb", _SH_DENYNO);
#else
    reader->file_ = std::fopen(path.c_str(), "rb");
#endif
    if (!reader->file_) return fail("can't open it");
    // every read seeks first, so a buffer only copies
    std::setvbuf(reader->file_, nullptr, _IONBF, 0);

    uint8_t header[kHeaderBytes];
    if (!reader->ReadAt(0, header, sizeof(header))) return fail("too short for a package");
    const uint32_t magic = Be32(header);
    if (magic != kCon && magic != kLive && magic != kPirs) return fail("not a package");
    if (Be32(header + kVolumeType) != 0) return fail("not an STFS package");
    const uint8_t* descriptor = header + kDescriptor;
    if (descriptor[0] != 0x24) return fail("no STFS volume descriptor");
    const uint32_t header_size = Be32(header + kHeaderSize);
    if (!header_size) return fail("no header size");
    reader->data_offset_ = (uint64_t(header_size) + kBlockSize - 1) / kBlockSize * kBlockSize;
    const uint8_t flags = descriptor[2];
    reader->tables_per_level_ = (flags & 1) ? 1 : 2;  // read_only_format: one copy of each table
    reader->root_second_copy_ = (flags & 2) != 0;
    const uint32_t table_blocks = uint32_t(descriptor[3] | descriptor[4] << 8);
    const uint32_t table_start = Le24(descriptor + 5);
    reader->total_blocks_ = Be32(descriptor + 0x1C);

    // the file table: its blocks are chained like a file's
    std::vector<uint32_t> blocks;
    {
        std::lock_guard lock(reader->mutex_);
        uint32_t block = table_start;
        for (uint32_t i = 0; i < table_blocks; i++) {
            if (block == kEndOfChain || block >= reader->total_blocks_) return fail("broken file table");
            blocks.push_back(block);
            if (i + 1 == table_blocks) break;
            uint32_t info;
            if (!reader->HashEntryInfo(block, &info)) return fail("can't read its hash tables");
            block = info & 0xFFFFFF;
        }
    }
    std::vector<uint8_t> table(kBlockSize);
    for (size_t b = 0; b < blocks.size(); b++) {
        std::lock_guard lock(reader->mutex_);
        if (!reader->ReadAt(reader->BlockOffset(blocks[b]), table.data(), table.size())) {
            return fail("can't read its file table");
        }
        for (size_t m = 0; m < kBlockSize / kEntrySize; m++) {
            const uint8_t* e = table.data() + m * kEntrySize;
            if (!e[0]) break;  // the rest of this block is empty, as the SDK reads it
            Node node;
            node.index = uint32_t(b * (kBlockSize / kEntrySize) + m);
            node.name.assign(reinterpret_cast<const char*>(e), e[0x28] & 0x3F);
            node.contiguous = (e[0x28] & 0x40) != 0;
            node.directory = (e[0x28] & 0x80) != 0;
            node.allocated_blocks = Le24(e + 0x2C);
            node.start_block = Le24(e + 0x2F);
            node.parent = Be16(e + 0x32);
            node.length = Be32(e + 0x34);
            node.create_date = Be16(e + 0x38);
            node.create_time = Be16(e + 0x3A);
            node.modified_date = Be16(e + 0x3C);
            node.modified_time = Be16(e + 0x3E);
            reader->nodes_.push_back(std::move(node));
        }
    }
    return reader;
}

uint64_t StfsReader::BlockOffset(uint32_t block) const {
    // each level's hash tables come before the blocks they cover
    uint64_t physical = block;
    for (int level = 0; level < 3; level++) {
        physical += uint64_t((block + kPerLevel[level]) / kPerLevel[level]) * tables_per_level_;
        if (block < kPerLevel[level]) break;
    }
    return data_offset_ + physical * kBlockSize;
}

uint64_t StfsReader::HashTableOffset(uint32_t block, int level) const {
    // a group's tables sit just before its first data block: its level 0
    // table last, the level 1 table before that and the level 2 table first;
    // the first level 1 table is the exception, after the first group of 170,
    // since the first level 0 table comes before it
    const uint64_t copies = tables_per_level_;
    if (level == 0) return BlockOffset(block / kPerLevel[0] * kPerLevel[0]) - copies * kBlockSize;
    if (level == 1) {
        const uint32_t first = std::max(block / kPerLevel[1] * kPerLevel[1], kPerLevel[0]);
        return BlockOffset(first) - 2 * copies * kBlockSize;
    }
    return BlockOffset(kPerLevel[1]) - 3 * copies * kBlockSize;
}

const uint8_t* StfsReader::HashTable(uint64_t offset) {
    auto it = tables_.find(offset);
    if (it != tables_.end()) return it->second.data();
    std::vector<uint8_t> table(kBlockSize);
    if (!ReadAt(offset, table.data(), table.size())) return nullptr;
    return tables_.emplace(offset, std::move(table)).first->second.data();
}

bool StfsReader::HashEntryInfo(uint32_t block, uint32_t* info) {
    const int levels = total_blocks_ <= kPerLevel[0] ? 1 : total_blocks_ <= kPerLevel[1] ? 2 : 3;
    // the top table's copy in use is in the header; each table's entries say
    // which copy of the table below them is
    bool second = tables_per_level_ == 2 && root_second_copy_;
    for (int level = levels - 1; level >= 0; level--) {
        const uint8_t* table = HashTable(HashTableOffset(block, level) + (second ? kBlockSize : 0));
        if (!table) return false;
        const uint32_t index = (level ? block / kPerLevel[level - 1] : block) % kPerLevel[0];
        const uint32_t entry = Be32(table + index * kHashEntrySize + 0x14);
        if (level == 0) {
            *info = entry;
            return true;
        }
        second = tables_per_level_ == 2 && (entry & kActiveIndex) != 0;
    }
    return false;
}

bool StfsReader::ChainBlocks(uint32_t start, uint32_t count, std::vector<uint32_t>* out) {
    std::lock_guard lock(mutex_);
    out->clear();
    out->reserve(count);
    uint32_t block = start;
    for (uint32_t i = 0; i < count; i++) {
        if (block == kEndOfChain || block >= total_blocks_) return false;
        out->push_back(block);
        if (i + 1 == count) break;
        uint32_t info;
        if (!HashEntryInfo(block, &info)) return false;
        block = info & 0xFFFFFF;
    }
    return true;
}

bool StfsReader::FileBlocks(const Node& node, Blocks* out) {
    *out = {};
    out->count = uint32_t((uint64_t(node.length) + kBlockSize - 1) / kBlockSize);
    if (!out->count) return true;
    if (node.contiguous && uint64_t(node.start_block) + out->count <= total_blocks_) {
        out->start = node.start_block;
        return true;
    }
    return ChainBlocks(node.start_block, out->count, &out->chain);
}

bool StfsReader::Read(const Blocks& blocks, uint64_t size, uint64_t offset, std::span<uint8_t> out,
                      size_t* got) {
    *got = 0;
    if (offset >= size) return true;
    const uint64_t want = std::min<uint64_t>(out.size(), size - offset);
    std::lock_guard lock(mutex_);
    uint64_t done = 0;
    while (done < want) {
        const uint64_t at = offset + done;
        uint32_t i = uint32_t(at / kBlockSize);
        if (i >= blocks.count) return false;
        const uint64_t start = BlockOffset(blocks.at(i)) + at % kBlockSize;
        // as far as the blocks run on in the file: up to the next hash table
        uint64_t run = kBlockSize - at % kBlockSize;
        while (done + run < want && i + 1 < blocks.count &&
               BlockOffset(blocks.at(i + 1)) == BlockOffset(blocks.at(i)) + kBlockSize) {
            run += kBlockSize;
            i++;
        }
        run = std::min(run, want - done);
        if (!ReadAt(start, out.data() + done, size_t(run))) return false;
        done += run;
    }
    *got = size_t(done);
    return true;
}

bool StfsReader::ReadAt(uint64_t offset, void* out, size_t size) {
    return Seek(file_, offset) == 0 && std::fread(out, 1, size, file_) == size;
}

}
