// Checks the web server's milo reader (src/Net/milo.cpp): milo files built
// here from made-up payloads, deflated with stb_image_write's zlib, so no
// game data is in the repository.

#include <doctest/doctest.h>
#include <cstdint>
#include <cstdlib>
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

// a milo: the header (magic, data offset 0x810, block count, largest block,
// block sizes), padding to the offset, then the blocks
std::string Milo(uint32_t magic, const std::vector<std::pair<uint32_t, std::string>>& blocks) {
    size_t largest = 0;
    for (const auto& [flags, b] : blocks) largest = std::max(largest, b.size());
    std::string out = Le32(magic) + Le32(0x810) + Le32(static_cast<uint32_t>(blocks.size())) +
                      Le32(static_cast<uint32_t>(largest));
    for (const auto& [flags, b] : blocks) out += Le32(static_cast<uint32_t>(b.size()) | flags);
    out.resize(0x810, '\0');
    for (const auto& [flags, b] : blocks) out += b;
    return out;
}

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
    // neither deflate nor a milo's start
    CHECK(!Decompress(Milo(0xCABEDEAF, {{0, std::string(64, '\xFF')}})));
}

TEST_CASE("what isn't a milo, or runs past its end, is refused") {
    CHECK(!Decompress(""));
    CHECK(!Decompress(Le32(0xCDBEDEAF) + Le32(0x810)));
    CHECK(!Decompress(Milo(0x12345678, {{kStored, kPayload}})));
    const std::string whole = Milo(0xCDBEDEAF, {{kStored, kPayload}});
    CHECK(!Decompress(whole.substr(0, whole.size() - 1)));
    // a block count far past what the header can hold
    std::string huge = Le32(0xCDBEDEAF) + Le32(0x810) + Le32(1000000) + Le32(16);
    huge.resize(0x810, '\0');
    CHECK(!Decompress(huge));
}
