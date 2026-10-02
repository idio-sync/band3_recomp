// Checks reading STFS package headers and finding packages in folders
// (src/Content/package_scan.cpp), with headers built here.

#include <doctest/doctest.h>
#include <filesystem>
#include <fstream>
#include <set>
#include <vector>
#include "src/Content/package_scan.h"

namespace fs = std::filesystem;
using namespace band3::content;

namespace {

void PutU32(std::vector<uint8_t>& b, size_t at, uint32_t v) {
    b[at] = uint8_t(v >> 24); b[at + 1] = uint8_t(v >> 16); b[at + 2] = uint8_t(v >> 8); b[at + 3] = uint8_t(v);
}

std::vector<uint8_t> MakeHeader(const char* magic, uint32_t type, uint32_t title, uint8_t id_byte,
                                const std::u16string& name) {
    std::vector<uint8_t> b(0x1000, 0);
    for (int i = 0; i < 4; i++) b[i] = uint8_t(magic[i]);
    for (int i = 0; i < 20; i++) b[0x32C + i] = uint8_t(id_byte + i);
    PutU32(b, 0x344, type);
    PutU32(b, 0x360, title);
    for (size_t i = 0; i < name.size(); i++) {
        b[0x411 + i * 2] = uint8_t(name[i] >> 8);
        b[0x411 + i * 2 + 1] = uint8_t(name[i]);
    }
    return b;
}

void Write(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                std::streamsize(bytes.size()));
}

}

TEST_CASE("a custom song's header reads back") {
    auto b = MakeHeader("CON ", 1, kRb3TitleId, 0xA0, u"Accept - Balls to The Wall");
    // licence 0: all bits, flags set; licence 1: bits but no flags, ignored
    PutU32(b, 0x22C + 8, 0xFFFFFFFF); PutU32(b, 0x22C + 12, 1);
    PutU32(b, 0x23C + 8, 0x0000F000); PutU32(b, 0x23C + 12, 0);
    auto h = ParsePackageHeader(b);
    REQUIRE(h);
    CHECK(h->magic == 0x434F4E20);
    CHECK(h->content_type == 1);
    CHECK(h->title_id == kRb3TitleId);
    CHECK(h->license_mask == 0xFFFFFFFF);
    CHECK(h->content_id == "A0A1A2A3A4A5A6A7A8A9AAABACADAEAFB0B1B2B3");
    CHECK(h->display_name == u"Accept - Balls to The Wall");
}

TEST_CASE("LIVE and PIRS are packages; anything else isn't") {
    CHECK(ParsePackageHeader(MakeHeader("LIVE", 2, kRb3TitleId, 1, u"x")));
    CHECK(ParsePackageHeader(MakeHeader("PIRS", 2, kRb3TitleId, 1, u"x")));
    CHECK_FALSE(ParsePackageHeader(MakeHeader("RIFF", 2, kRb3TitleId, 1, u"x")));
    auto short_file = MakeHeader("CON ", 1, kRb3TitleId, 1, u"x");
    short_file.resize(kHeaderBytes - 1);
    CHECK_FALSE(ParsePackageHeader(short_file));
}

TEST_CASE("a display name filling all 128 characters has no terminator") {
    auto h = ParsePackageHeader(MakeHeader("CON ", 1, kRb3TitleId, 1, std::u16string(128, u'a')));
    REQUIRE(h);
    CHECK(h->display_name.size() == 128);
}

TEST_CASE("scanning keeps one of each RB3 package and skips the rest") {
    const fs::path root = fs::temp_directory_path() / "band3_scan_test";
    fs::remove_all(root);
    const auto song = MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"Song");
    Write(root / "a" / "Song_rb3con", song);
    Write(root / "a" / "sub" / "Song copy", song);              // same content ID: once
    Write(root / "a" / "Other", MakeHeader("CON ", 1, kRb3TitleId, 0x40, u"Other"));
    Write(root / "a" / "rb2_song", MakeHeader("CON ", 2, 0x45410869, 0x70, u"RB2"));  // RB3 reads RB2's
    Write(root / "a" / "forza", MakeHeader("CON ", 2, 0x4D5307E6, 0x90, u"Car"));      // another game's
    Write(root / "a" / "notes.txt", std::vector<uint8_t>(10, 'x'));                    // junk
    Write(root / "a" / "Coming_1.part", MakeHeader("CON ", 1, kRb3TitleId, 0xB0, u"Coming"));  // downloading
    std::vector<std::string> problems;
    auto found = ScanFolders({root / "a", root / "missing"}, kRb3TitleIds, &problems);
    REQUIRE(found.size() == 3);
    std::set<std::u16string> names;
    for (const auto& package : found) names.insert(package.header.display_name);
    CHECK(names == std::set<std::u16string>{u"Song", u"Other", u"RB2"});
    // the missing folder is reported by name; junk and the other title aren't problems
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("missing") != std::string::npos);
    fs::remove_all(root);
}

TEST_CASE("the first folder listed wins a content ID found in two") {
    const fs::path root = fs::temp_directory_path() / "band3_scan_order_test";
    fs::remove_all(root);
    Write(root / "first" / "song", MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"First"));
    Write(root / "second" / "song", MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"Second"));
    Write(root / "second" / "other", MakeHeader("CON ", 1, kRb3TitleId, 0x40, u"Other"));
    auto found = ScanFolders({root / "first", root / "second"}, kRb3TitleIds, nullptr);
    REQUIRE(found.size() == 2);
    CHECK(found[0].header.display_name == u"First");
    CHECK(found[1].header.display_name == u"Other");
    fs::remove_all(root);
}

#ifdef _WIN32
TEST_CASE("a subfolder that can't be listed is a problem, and the rest of its folder is still read") {
    // a path past MAX_PATH can be made through \\?\ but not listed without it
    const fs::path root = fs::temp_directory_path() / "band3_scan_long_test";
    const fs::path long_root = L"\\\\?\\" + root.wstring();
    fs::remove_all(long_root);
    fs::create_directories(long_root / "a" / "0deep" / std::wstring(240, L'x'));
    Write(root / "a" / "z_song", MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"After"));
    std::vector<std::string> problems;
    auto found = ScanFolders({root / "a"}, kRb3TitleIds, &problems);
    REQUIRE(found.size() == 1);
    CHECK(found[0].header.display_name == u"After");
    REQUIRE(problems.size() == 1);
    CHECK(problems[0].find("0deep") != std::string::npos);
    fs::remove_all(long_root);
}
#endif

TEST_CASE("one file reads as a package when it's an RB3 one, whole") {
    const fs::path root = fs::temp_directory_path() / "band3_read_package_test";
    fs::remove_all(root);
    Write(root / "Song", MakeHeader("CON ", 1, kRb3TitleId, 0x10, u"Song"));
    Write(root / "Song_x.part", MakeHeader("CON ", 1, kRb3TitleId, 0x20, u"Coming"));
    Write(root / "forza", MakeHeader("CON ", 2, 0x4D5307E6, 0x90, u"Car"));
    Write(root / "short", std::vector<uint8_t>(16, 'x'));
    const auto song = ReadPackage(root / "Song", kRb3TitleIds);
    REQUIRE(song);
    CHECK(song->path == root / "Song");
    CHECK(song->header.display_name == u"Song");
    CHECK(!ReadPackage(root / "Song_x.part", kRb3TitleIds));  // still downloading
    CHECK(!ReadPackage(root / "forza", kRb3TitleIds));        // another game's
    CHECK(!ReadPackage(root / "short", kRb3TitleIds));
    CHECK(!ReadPackage(root / "missing", kRb3TitleIds));
    fs::remove_all(root);
}
