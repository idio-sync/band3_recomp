// Checks the web server's milo reader (src/Net/milo.cpp): milo files built
// here from made-up payloads, deflated with stb_image_write's zlib, so no
// game data is in the repository.

#include <doctest/doctest.h>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "src/Net/milo.h"

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "src/ThirdParty/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

using namespace band3::milo;

namespace {

std::string Le32(uint32_t v) {
    std::string out;
    for (int i = 0; i < 4; i++) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}

// raw deflate: stb's zlib stream without its 2-byte header and 4-byte Adler-32
std::string Deflate(const std::string& data) {
    int size = 0;
    unsigned char* z = stbi_zlib_compress(
        reinterpret_cast<unsigned char*>(const_cast<char*>(data.data())),
        static_cast<int>(data.size()), &size, 8);
    std::string out(reinterpret_cast<char*>(z) + 2, static_cast<size_t>(size) - 6);
    STBIW_FREE(z);
    return out;
}

constexpr uint32_t kStored = 0x01000000;

// a milo: the header (magic, data offset 0x810, block count, the largest
// block once inflated, block sizes), padding to the offset, then the blocks
std::string Milo(uint32_t magic, const std::vector<std::pair<uint32_t, std::string>>& blocks,
                 uint32_t largest = 0x10000) {
    std::string out = Le32(magic) + Le32(0x810) + Le32(static_cast<uint32_t>(blocks.size())) +
                      Le32(largest);
    for (const auto& [flags, b] : blocks) out += Le32(static_cast<uint32_t>(b.size()) | flags);
    out.resize(0x810, '\0');
    for (const auto& [flags, b] : blocks) out += b;
    return out;
}

// big-endian writing, as the 360 saves a milo's objects
std::string Be32(uint32_t v) {
    std::string out;
    for (int i = 3; i >= 0; i--) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}
std::string Be16(uint16_t v) { return {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; }
std::string BeF(float f) {
    uint32_t v;
    std::memcpy(&v, &f, 4);
    return Be32(v);
}
std::string Name(const std::string& s) { return Be32(static_cast<uint32_t>(s.size())) + s; }

struct Info {
    uint16_t code;
    float u, v, width, advance;
};

// a Font object's data from its material's name on (milo.h's FindFont)
std::string FontData(const std::string& owner, const std::vector<uint16_t>& chars,
                     const std::vector<Info>& infos, bool kerning) {
    std::string out = Name("a.mat") + BeF(30) + BeF(20) + BeF(0) + BeF(0.125f);
    out += Be32(static_cast<uint32_t>(chars.size()));
    for (uint16_t c : chars) out += Be16(c);
    out += kerning ? std::string(1, '\x01') + Be32(1) + Be32('A' | 'B' << 16) + BeF(-0.25f)
                   : std::string(1, '\0');
    out += Name(owner) + std::string(1, '\0') + std::string(1, '\x01');  // monospace, packed
    out += Be32(256) + Be32(128) + BeF(30.0f / 256) + BeF(20.0f / 128);
    out += Be32(static_cast<uint32_t>(infos.size()));
    for (const Info& i : infos) out += Be16(i.code) + BeF(i.u) + BeF(i.v) + BeF(i.width) + BeF(i.advance);
    return out + Name("");
}

const std::vector<Info> kInfos = {
    {'A', 0, 0, 0.75f, 0.8f}, {'B', 0.125f, 0, 0.5f, 0.625f}, {' ', 0.5f, 0.5f, 0, 0.375f}};

// what a decompressed milo starts with: its version, big-endian
const std::string kPayload = std::string("\x00\x00\x00\x1c", 4) + "UILabelDir and the rest " +
                             std::string(3000, 'x') + "end";

}

TEST_CASE("CDBEDEAF blocks inflate after their 4-byte size, and stored ones are copied") {
    const std::string a = kPayload.substr(0, 1000), b = kPayload.substr(1000);
    CHECK(Decompress(Milo(0xCDBEDEAF, {{0, Le32(1000) + Deflate(a)},
                                       {0, Le32(static_cast<uint32_t>(b.size())) + Deflate(b)}})) ==
          kPayload);
    CHECK(Decompress(Milo(0xCDBEDEAF, {{0, Le32(1000) + Deflate(a)}, {kStored, b}})) == kPayload);
}

TEST_CASE("CABEDEAF blocks inflate as they are, or are taken as stored when they start a milo") {
    CHECK(Decompress(Milo(0xCABEDEAF, {{0, Deflate(kPayload)}})) == kPayload);
    // Deluxe writes some uncompressed without the stored flag
    CHECK(Decompress(Milo(0xCABEDEAF, {{0, kPayload}})) == kPayload);
    // and once one is, the blocks after it, which don't start one
    CHECK(Decompress(Milo(0xCABEDEAF, {{0, kPayload.substr(0, 1000)}, {0, kPayload.substr(1000)}})) ==
          kPayload);
    // neither deflate nor a milo's start
    CHECK(!Decompress(Milo(0xCABEDEAF, {{0, std::string(64, '\xFF')}})));
}

TEST_CASE("a block that inflates past the largest block the header gives is refused") {
    // 64 KB of one byte deflates to almost nothing; the header says blocks are
    // at most 1 KB once inflated
    const std::string big(64 * 1024, 'x');
    std::string milo = Milo(0xCDBEDEAF, {{0, Le32(static_cast<uint32_t>(big.size())) + Deflate(big)}});
    milo.replace(12, 4, Le32(1024));
    CHECK(!Decompress(milo));
    // as the game writes them, the largest block given is the largest inflated
    std::string fits = Milo(0xCABEDEAF, {{0, Deflate(big)}});
    fits.replace(12, 4, Le32(static_cast<uint32_t>(big.size())));
    CHECK(Decompress(fits) == big);
}

TEST_CASE("what isn't a milo, or runs past its end, is refused") {
    CHECK(!Decompress(""));
    CHECK(!Decompress(Le32(0xCDBEDEAF) + Le32(0x810)));
    CHECK(!Decompress(Milo(0x12345678, {{kStored, kPayload}})));
    const std::string whole = Milo(0xCDBEDEAF, {{kStored, kPayload}});
    CHECK(!Decompress(whole.substr(0, whole.size() - 1)));
    // a data offset inside the header itself
    std::string inside = Le32(0xCDBEDEAF) + Le32(8) + Le32(1) + Le32(16);
    CHECK(!Decompress(inside));
    // a block count far past what the header can hold
    std::string huge = Le32(0xCDBEDEAF) + Le32(0x810) + Le32(1000000) + Le32(16);
    huge.resize(0x810, '\0');
    CHECK(!Decompress(huge));
}

TEST_CASE("the font with the most glyphs is found among the milo's objects") {
    // a placeholder that lists characters but has no glyphs, then the font
    const std::string raw = std::string("\x00\x00\x00\x1c junk .mat here", 19) +
                            FontData("other.font", {'A'}, {}, false) + "between" +
                            FontData("a.font", {'A', 'B', ' '}, kInfos, true) + "after";
    const auto font = FindFont(raw);
    REQUIRE(font);
    CHECK(font->cell_w == 30);
    CHECK(font->cell_h == 20);
    CHECK(font->base_kerning == 0.125f);
    CHECK(font->texture_w == 256);
    CHECK(font->texture_h == 128);
    CHECK(!font->monospace);
    REQUIRE(font->glyphs.size() == 3);
    CHECK(font->glyphs[1].code == 'B');
    CHECK(font->glyphs[1].u == 0.125f);
    CHECK(font->glyphs[1].width == 0.5f);
    CHECK(font->glyphs[1].advance == 0.625f);
    REQUIRE(font->kerning.size() == 1);
    CHECK(font->kerning[0].left == 'A');
    CHECK(font->kerning[0].right == 'B');
    CHECK(font->kerning[0].kerning == -0.25f);
}

TEST_CASE("a font cut short anywhere, or with lengths past its end, isn't read") {
    const std::string whole = FontData("a.font", {'A', 'B', ' '}, kInfos, true);
    for (size_t n = 0; n < whole.size(); n++) CHECK(!FindFont(whole.substr(0, n)));
    CHECK(FindFont(whole));
    // a name 4 GB long
    std::string huge = whole;
    huge.replace(huge.find("a.font") - 4, 4, Be32(0xFFFFFFFF));
    CHECK(!FindFont(huge));
    CHECK(!FindFont(""));
}

TEST_CASE("an embedded bitmap decodes, its header read big-endian") {
    // a 4x4 DXT5 bitmap: alpha 255 everywhere, color 0 red
    std::string block = {'\xFF', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
                         '\0', '\xF8', '\0', '\0', '\0', '\0', '\0', '\0'};
    // the 360's byte-swapped 16-bit words
    for (size_t i = 0; i < block.size(); i += 2) std::swap(block[i], block[i + 1]);
    const std::string header = std::string("\x01\x08", 2) + Be32(24) + std::string(1, '\0') +
                               Be16(4) + Be16(4) + Be16(4) + std::string(19, '\0');
    const auto image = FindBitmap("before" + header + block);
    REQUIRE(image);
    CHECK(image->width == 4);
    CHECK(image->height == 4);
    CHECK(image->rgba[0] == 255);  // red
    CHECK(image->rgba[2] == 0);
    CHECK(image->rgba[3] == 255);  // opaque
    CHECK(!FindBitmap("no bitmap here"));
}

TEST_CASE("a font's JSON gives its cells, texture, glyphs and kerning") {
    Font font;
    font.cell_w = 30;
    font.cell_h = 20;
    font.base_kerning = 0.125f;
    font.texture_w = 256;
    font.texture_h = 128;
    font.glyphs = {{'A', 0, 0.5f, 0.75f, 1}};
    font.kerning = {{'A', 'B', -0.25f}};
    CHECK(FormatFontJson(font) ==
          "{\"cell\":[30,20],\"texture\":[256,128],\"base_kerning\":0.125,\"monospace\":false,"
          "\"glyphs\":{\"65\":[0,0.5,0.75,1]},\"kerning\":[[65,66,-0.25]]}");
}
