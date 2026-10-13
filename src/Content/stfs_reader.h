// Reads files out of an Xbox 360 STFS package (CON/LIVE/PIRS) without the
// SDK's StfsContainerDevice, which walks every hash table in the package when
// it opens one: a few thousand 4 KB reads for a 2-3 GB pack, each a round
// trip on a network share. This reads the header and the file table, and a
// file's blocks only when it's opened; a file stored in one run of blocks
// (the table's contiguous flag, as packages are built) needs no hash tables
// at all.
#pragma once
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

namespace band3::content {

class StfsReader {
public:
    static constexpr uint32_t kBlockSize = 0x1000;
    static constexpr uint16_t kNoParent = 0xFFFF;

    // one entry of the package's file table
    struct Node {
        uint32_t index = 0;  // its place in the table, which a child's parent names
        std::string name;
        bool directory = false;
        bool contiguous = false;
        uint32_t start_block = 0;
        uint32_t length = 0;
        uint32_t allocated_blocks = 0;
        uint16_t parent = kNoParent;  // the table index of its folder, or kNoParent for the root
        uint16_t create_date = 0, create_time = 0;  // FAT date and time
        uint16_t modified_date = 0, modified_time = 0;
    };

    // a file's data blocks, in order
    struct Blocks {
        uint32_t count = 0;
        uint32_t start = 0;            // when chain is empty: blocks start .. start + count - 1
        std::vector<uint32_t> chain;   // otherwise, the blocks themselves
        uint32_t at(uint32_t i) const { return chain.empty() ? start + i : chain[i]; }
    };

    ~StfsReader();

    // the package at path, its file table read; nullptr (and why, in error)
    // when it isn't an STFS package (SVOD isn't) or can't be read
    static std::unique_ptr<StfsReader> Open(const std::filesystem::path& path, std::string* error);

    const std::vector<Node>& nodes() const { return nodes_; }
    uint32_t total_blocks() const { return total_blocks_; }

    // node's blocks: counted from its start for a contiguous file, else
    // followed through the hash tables; false if the chain is broken
    bool FileBlocks(const Node& node, Blocks* out);
    // the chain from start, followed through the hash tables whatever the
    // contiguous flag says (for checking it)
    bool ChainBlocks(uint32_t start, uint32_t count, std::vector<uint32_t>* out);

    // reads out.size() bytes of a file of `size` bytes at offset; *got is
    // what was read (less at the file's end)
    bool Read(const Blocks& blocks, uint64_t size, uint64_t offset, std::span<uint8_t> out,
              size_t* got);

    // the byte offset of data block `block` in the package file
    uint64_t BlockOffset(uint32_t block) const;

private:
    StfsReader() = default;
    bool ReadAt(uint64_t offset, void* out, size_t size);  // mutex_ held
    // the hash table entry for data block `block`; mutex_ held
    bool HashEntryInfo(uint32_t block, uint32_t* info);
    // the byte offset of the level `level` hash table covering `block` (its first copy)
    uint64_t HashTableOffset(uint32_t block, int level) const;
    const uint8_t* HashTable(uint64_t offset);  // read once, then kept; mutex_ held

    std::FILE* file_ = nullptr;
    std::mutex mutex_;
    uint64_t data_offset_ = 0;  // where block numbering starts: the header, rounded up to a block
    uint32_t tables_per_level_ = 2;  // 1 in a read-only package, else 2 copies of each table
    bool root_second_copy_ = false;  // the top table's second copy is the one in use
    uint32_t total_blocks_ = 0;
    std::vector<Node> nodes_;
    std::unordered_map<uint64_t, std::vector<uint8_t>> tables_;
};

}
