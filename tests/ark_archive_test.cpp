// Checks reading Rock Band 3's archive without the game
// (src/Content/ark_archive.cpp): the DTB cipher against
// tools/read_game_config.py's, and a made-up archive of two parts. A test
// skipped by default decrypts every disc song in assets/gen (mogg_crypt.cpp):
// run it with --no-skip.

#include <doctest/doctest.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include "src/Content/ark_archive.h"
#include "src/Video/mogg.h"
#include "src/Video/mogg_crypt.h"

using namespace band3::content;

namespace {

using Bytes = std::vector<uint8_t>;

void Le32(Bytes& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
}

void Text(Bytes& out, const std::string& s) {
    Le32(out, uint32_t(s.size()));
    out.insert(out.end(), s.begin(), s.end());
}

Bytes Hex(const std::string& hex) {
    Bytes out;
    for (size_t i = 0; i + 1 < hex.size(); i += 2)
        out.push_back(uint8_t(std::stoi(hex.substr(i, 2), nullptr, 16)));
    return out;
}

// `plain` ciphered with this seed (the cipher is its own inverse)
Bytes Cipher(int32_t seed, const Bytes& plain) {
    Bytes seeded;
    Le32(seeded, uint32_t(seed));
    Bytes out = seeded;
    seeded.insert(seeded.end(), plain.begin(), plain.end());
    const Bytes xored = DecryptDtb(seeded);
    out.insert(out.end(), xored.begin(), xored.end());
    return out;
}

void Write(const std::filesystem::path& path, const Bytes& data) {
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), data.size());
}

struct File {
    uint64_t offset;
    int32_t name, dir;
    uint32_t size;
};

// a version 6 header: parts of these sizes, names in a blob, these files
Bytes Header(const std::vector<uint32_t>& sizes, const std::vector<std::string>& strings,
             const std::vector<File>& files, int32_t version = 6) {
    Bytes h;
    Le32(h, uint32_t(version));
    Le32(h, 1);
    h.insert(h.end(), 16, 0);
    Le32(h, uint32_t(sizes.size()));
    Le32(h, uint32_t(sizes.size()));
    for (uint32_t s : sizes) Le32(h, s);
    Le32(h, uint32_t(sizes.size()));
    for (size_t i = 0; i < sizes.size(); i++) Text(h, "gen/test_" + std::to_string(i) + ".ark");
    Le32(h, 0);  // checksums
    Bytes blob;
    std::vector<uint32_t> offsets;
    for (const std::string& s : strings) {
        offsets.push_back(uint32_t(blob.size()));
        blob.insert(blob.end(), s.begin(), s.end());
        blob.push_back(0);
    }
    Le32(h, uint32_t(blob.size()));
    h.insert(h.end(), blob.begin(), blob.end());
    Le32(h, uint32_t(offsets.size()));
    for (uint32_t o : offsets) Le32(h, o);
    Le32(h, uint32_t(files.size()));
    for (const File& f : files) {
        Le32(h, uint32_t(f.offset));
        Le32(h, uint32_t(f.offset >> 32));
        Le32(h, uint32_t(f.name));
        Le32(h, uint32_t(f.dir));
        Le32(h, f.size);
        Le32(h, f.size);
    }
    return h;
}

// a folder of its own under the system's temp folder, removed after
struct TempDir {
    std::filesystem::path path;
    TempDir() {
        path = std::filesystem::temp_directory_path() / "band3_ark_archive_test";
        std::filesystem::remove_all(path);
        std::filesystem::create_directories(path);
    }
    ~TempDir() { std::filesystem::remove_all(path); }
};

}

TEST_CASE("the DTB cipher's stream matches read_game_config.py's") {
    // read_game_config.py's decrypt of each seed and eight zero bytes
    CHECK(DecryptDtb(Hex("78563412" "0000000000000000")) == Hex("9ea9fa9b023ba937"));
    CHECK(DecryptDtb(Hex("ccedffff" "0000000000000000")) == Hex("13a72a1ee563440c"));
    CHECK(DecryptDtb(Hex("00000000" "0000000000000000")) == Hex("ffffffffffffffff"));
    CHECK(DecryptDtb(Hex("000000")).empty());
}

TEST_CASE("a made-up archive's files read from their parts") {
    TempDir dir;
    // the parts' bytes count up across both, so each file's bytes say where they were
    Bytes part0(100), part1(50);
    for (size_t i = 0; i < part0.size(); i++) part0[i] = uint8_t(i);
    for (size_t i = 0; i < part1.size(); i++) part1[i] = uint8_t(100 + i);
    Write(dir.path / "test_0.ark", part0);
    Write(dir.path / "test_1.ark", part1);
    const std::vector<std::string> strings = {"songs/x", "x.mogg", "Songs/Y", "Y.mogg", "z.mogg"};
    const std::vector<File> files = {
        {10, 1, 0, 20},   // in the first part
        {90, 3, 2, 30},   // across both
        {140, 4, 0, 20},  // past the end
    };
    Write(dir.path / "test.hdr", Cipher(0x1234567, Header({100, 50}, strings, files)));

    std::string error;
    const std::optional<ArkArchive> ark = ArkArchive::Open(dir.path / "test.hdr", error);
    REQUIRE_MESSAGE(ark, error);
    CHECK(ark->Has("songs/x/x.mogg"));
    CHECK(ark->Has("SONGS/y/y.MOGG"));
    CHECK_FALSE(ark->Has("songs/x/y.mogg"));

    Bytes out;
    REQUIRE(ark->Read("songs/x/x.mogg", out, error));
    REQUIRE(out.size() == 20);
    CHECK(out.front() == 10);
    CHECK(out.back() == 29);
    REQUIRE(ark->Read("songs/y/y.mogg", out, error));
    REQUIRE(out.size() == 30);
    CHECK(out.front() == 90);
    CHECK(out[9] == 99);
    CHECK(out[10] == 100);
    CHECK(out.back() == 119);
    CHECK_FALSE(ark->Read("songs/x/z.mogg", out, error));
    CHECK(error == "songs/x/z.mogg runs past the archive's end");
    CHECK_FALSE(ark->Read("songs/q/q.mogg", out, error));
    CHECK(error == "the archive has no songs/q/q.mogg");
}

TEST_CASE("an archive that isn't version 6, or is cut short, isn't opened") {
    TempDir dir;
    std::string error;
    Write(dir.path / "old.hdr", Cipher(42, Header({10}, {"a", "b"}, {}, 5)));
    CHECK_FALSE(ArkArchive::Open(dir.path / "old.hdr", error));
    CHECK(error == "its header is version 5; band3 reads only 6");
    Bytes cut = Cipher(42, Header({10}, {"a", "b"}, {{0, 1, 0, 4}}));
    cut.resize(cut.size() - 6);
    Write(dir.path / "cut.hdr", cut);
    CHECK_FALSE(ArkArchive::Open(dir.path / "cut.hdr", error));
    CHECK(error == "its header is cut short");
    CHECK_FALSE(ArkArchive::Open(dir.path / "missing.hdr", error));
}

TEST_CASE("disc songs in assets/gen decrypt to a whole Ogg stream" * doctest::skip()) {
    const std::filesystem::path header = std::filesystem::path(BAND3_ROOT_DIR) / "assets/gen/main_xbox.hdr";
    std::string error;
    const std::optional<ArkArchive> ark = ArkArchive::Open(header, error);
    REQUIRE_MESSAGE(ark, error);
    for (const char* name : {"20thcenturyboy", "yoshimibattles", "thepoweroflove", "whipit"}) {
        CAPTURE(name);
        const std::string path = std::string("songs/") + name + "/" + name + ".mogg";
        Bytes mogg;
        REQUIRE_MESSAGE(ark->Read(path, mogg, error), error);
        REQUIRE(mogg[0] == 0x10);
        REQUIRE_MESSAGE(band3::video::DecryptMogg(mogg, error), error);
        const auto h = band3::video::ParseMoggHeader(mogg);
        REQUIRE(h);
        // its pages all read, to the end
        band3::video::OggPackets packets(std::span(mogg).subspan(h->ogg_offset));
        Bytes packet;
        size_t count = 0, bytes = 0;
        while (packets.Next(packet)) {
            count++;
            bytes += packet.size();
        }
        CHECK(count > 1000);
        CHECK(bytes > mogg.size() * 9 / 10);
    }
}
