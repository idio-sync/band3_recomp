// Checks reading files out of STFS packages (src/Content/stfs_reader.cpp),
// with packages built here. Where the hash tables and data blocks sit comes
// from the X360 library's formulas (StfsPackage::ComputeDataBlockNumber and
// the level backing hash block numbers), not from the reader's own.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>
#include "src/Content/stfs_reader.h"

namespace fs = std::filesystem;
using band3::content::StfsReader;

namespace {

constexpr uint32_t kHeaderSize = 0x971A;  // data from 0xA000
constexpr uint64_t kDataStart = 0xA000;
constexpr uint32_t kEnd = 0xFFFFFF;

void PutBe32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}
void PutBe16(std::vector<uint8_t>& b, size_t at, uint16_t v) { b[at] = uint8_t(v >> 8); b[at + 1] = uint8_t(v); }
void PutLe24(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v); b[at + 1] = uint8_t(v >> 8); b[at + 2] = uint8_t(v >> 16);
}

// sex 0: one copy of each hash table (read-only packages); 1: two
uint32_t DataBlock(uint32_t b, uint32_t sex) {
    uint32_t r = (((b + 0xAA) / 0xAA) << sex) + b;
    if (b < 0xAA) return r;
    return r + (((b + 0x70E4) / 0x70E4) << sex);  // packages here stay under 0x70E4 blocks
}
uint32_t Step0(uint32_t sex) { return 0xAA + (1u << sex); }
uint32_t Level0Table(uint32_t b, uint32_t sex) {
    if (b < 0xAA) return 0;
    return (b / 0xAA) * Step0(sex) + (((b / 0x70E4) + 1) << sex);
}
uint32_t Level1Table(uint32_t sex) { return Step0(sex); }

struct FileSpec {
    std::string name;
    uint16_t parent = 0xFFFF;
    bool directory = false;
    bool contiguous = false;
    std::vector<uint32_t> blocks;
    uint32_t length = 0;
};

uint8_t Pattern(uint32_t block, uint32_t at) { return uint8_t(block * 7 + at * 13 + (at >> 8)); }

struct Built {
    std::vector<uint8_t> bytes;
    std::map<std::string, std::vector<uint8_t>> contents;  // by name
};

// a package of total_blocks data blocks; block 0 is the file table, the
// others hold Pattern bytes. With two copies, the second copy of each table
// is the one in use (the first holds wrong next blocks), as the header and
// the level 1 table say.
Built Build(uint32_t sex, uint32_t total_blocks, const std::vector<FileSpec>& files) {
    Built out;
    const uint32_t last = DataBlock(total_blocks - 1, sex);
    out.bytes.assign(kDataStart + (uint64_t(last) + 1) * 0x1000, 0);
    auto& b = out.bytes;
    b[0] = 'C'; b[1] = 'O'; b[2] = 'N'; b[3] = ' ';
    PutBe32(b, 0x340, kHeaderSize);
    b[0x379] = 0x24;
    b[0x379 + 2] = uint8_t((sex == 0 ? 1 : 0) | (sex == 1 ? 2 : 0));  // read_only_format; root_active_index
    b[0x379 + 3] = 1;  // one file table block
    PutLe24(b, 0x379 + 5, 0);
    PutBe32(b, 0x379 + 0x1C, total_blocks);
    PutBe32(b, 0x3A9, 0);  // STFS

    auto data = [&](uint32_t block) { return kDataStart + uint64_t(DataBlock(block, sex)) * 0x1000; };
    for (uint32_t block = 1; block < total_blocks; block++) {
        for (uint32_t at = 0; at < 0x1000; at++) b[data(block) + at] = Pattern(block, at);
    }
    const uint64_t copy = sex == 1 ? 0x1000 : 0;  // the copy in use
    // the level 1 table, when there is one, says each level 0 table's second copy is in use
    if (total_blocks > 0xAA) {
        const uint64_t l1 = kDataStart + uint64_t(Level1Table(sex)) * 0x1000 + copy;
        for (uint32_t group = 0; group * 0xAA < total_blocks; group++) {
            PutBe32(b, l1 + group * 0x18 + 0x14, sex == 1 ? 0x40000000 : 0);
        }
    }
    auto set_next = [&](uint32_t block, uint32_t next) {
        const uint64_t table = kDataStart + uint64_t(Level0Table(block, sex)) * 0x1000;
        PutBe32(b, table + copy + (block % 0xAA) * 0x18 + 0x14, 0x80000000 | next);
        // the copy not in use points somewhere wrong
        if (sex == 1) PutBe32(b, table + (block % 0xAA) * 0x18 + 0x14, 0x80000000 | ((next + 5) % total_blocks));
    };
    set_next(0, kEnd);
    for (size_t i = 0; i < files.size(); i++) {
        const FileSpec& f = files[i];
        const uint64_t e = data(0) + i * 0x40;
        for (size_t c = 0; c < f.name.size(); c++) b[e + c] = uint8_t(f.name[c]);
        b[e + 0x28] = uint8_t(f.name.size() | (f.contiguous ? 0x40 : 0) | (f.directory ? 0x80 : 0));
        PutLe24(b, e + 0x29, uint32_t(f.blocks.size()));
        PutLe24(b, e + 0x2C, uint32_t(f.blocks.size()));
        PutLe24(b, e + 0x2F, f.blocks.empty() ? 0 : f.blocks[0]);
        PutBe16(b, e + 0x32, f.parent);
        PutBe32(b, e + 0x34, f.length);
        for (size_t k = 0; k < f.blocks.size(); k++) {
            set_next(f.blocks[k], k + 1 < f.blocks.size() ? f.blocks[k + 1] : kEnd);
        }
        std::vector<uint8_t> content;
        for (uint32_t block : f.blocks) {
            for (uint32_t at = 0; at < 0x1000; at++) content.push_back(Pattern(block, at));
        }
        content.resize(f.length);
        out.contents[f.name] = content;
    }
    return out;
}

fs::path WriteTemp(const std::string& name, const std::vector<uint8_t>& bytes) {
    const fs::path path = fs::temp_directory_path() / ("band3_stfs_test_" + name);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                std::streamsize(bytes.size()));
    return path;
}

std::vector<uint32_t> Range(uint32_t first, uint32_t count) {
    std::vector<uint32_t> out;
    for (uint32_t i = 0; i < count; i++) out.push_back(first + i);
    return out;
}

const StfsReader::Node* Find(StfsReader& reader, const std::string& name) {
    for (const auto& node : reader.nodes()) {
        if (node.name == name) return &node;
    }
    return nullptr;
}

std::vector<uint8_t> ReadWhole(StfsReader& reader, const StfsReader::Node& node, uint64_t offset = 0,
                               size_t size = SIZE_MAX) {
    StfsReader::Blocks blocks;
    REQUIRE(reader.FileBlocks(node, &blocks));
    std::vector<uint8_t> out(std::min<size_t>(size, node.length));
    size_t got = 0;
    REQUIRE(reader.Read(blocks, node.length, offset, out, &got));
    out.resize(got);
    return out;
}

void CheckPackage(uint32_t sex, const char* file_name) {
    // blocks 1..399: a contiguous file over the first group boundaries, a
    // chained one out of order, and one in a folder
    std::vector<uint32_t> scattered = {350, 310, 399, 305, 303, 380};
    const std::vector<FileSpec> files = {
        {"songs", 0xFFFF, true, false, {}, 0},
        {"songs.dta", 0, false, false, {2}, 1234},
        {"big.mogg", 0xFFFF, false, true, Range(3, 300), 300 * 0x1000 - 17},
        {"chained.mid", 0xFFFF, false, false, scattered, 5 * 0x1000 + 1},
    };
    const Built built = Build(sex, 400, files);
    const fs::path path = WriteTemp(file_name, built.bytes);
    std::string error;
    auto reader = StfsReader::Open(path, &error);
    REQUIRE_MESSAGE(reader, error);
    REQUIRE(reader->nodes().size() == 4);
    const auto* songs = Find(*reader, "songs");
    REQUIRE(songs);
    CHECK(songs->directory);
    const auto* dta = Find(*reader, "songs.dta");
    REQUIRE(dta);
    CHECK(dta->parent == songs->index);
    for (const auto& f : files) {
        if (f.directory) continue;
        const auto* node = Find(*reader, f.name);
        REQUIRE(node);
        CHECK(node->length == f.length);
        CHECK(ReadWhole(*reader, *node) == built.contents.at(f.name));
        // the chain through the hash tables gives the same blocks as the table
        std::vector<uint32_t> chain;
        REQUIRE(reader->ChainBlocks(node->start_block, uint32_t(f.blocks.size()), &chain));
        CHECK(chain == f.blocks);
    }
    // a read in the middle, across a hash table, and one past the end
    const auto* big = Find(*reader, "big.mogg");
    const auto& want = built.contents.at("big.mogg");
    const uint64_t at = 166 * 0x1000 + 100;
    const auto middle = ReadWhole(*reader, *big, at, 0x6000);
    CHECK(middle == std::vector<uint8_t>(want.begin() + at, want.begin() + at + 0x6000));
    const auto tail = ReadWhole(*reader, *big, big->length - 10, 100);
    CHECK(tail == std::vector<uint8_t>(want.end() - 10, want.end()));
    reader.reset();
    fs::remove(path);
}

}

TEST_CASE("StfsReader reads files from a package with two copies of each hash table") {
    CheckPackage(1, "male.con");
}

TEST_CASE("StfsReader reads files from a read-only package") {
    CheckPackage(0, "female.con");
}

TEST_CASE("StfsReader turns down what isn't an STFS package") {
    std::vector<uint8_t> bytes(0x2000, 0);
    std::string error;
    const fs::path junk = WriteTemp("junk", bytes);
    CHECK_FALSE(StfsReader::Open(junk, &error));
    CHECK(error == "not a package");
    bytes[0] = 'L'; bytes[1] = 'I'; bytes[2] = 'V'; bytes[3] = 'E';
    PutBe32(bytes, 0x3A9, 1);  // SVOD
    const fs::path svod = WriteTemp("svod", bytes);
    CHECK_FALSE(StfsReader::Open(svod, &error));
    CHECK(error == "not an STFS package");
    fs::remove(junk);
    fs::remove(svod);
}
