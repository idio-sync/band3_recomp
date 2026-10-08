// Checks album art decoding (src/Net/album_art.cpp): RB3's .png_xbox bitmaps,
// built here block by block, and the JPEGs the web server sends.

#include <doctest/doctest.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "src/Net/album_art.h"

using namespace band3::http;

namespace {

// a .png_xbox: the 32-byte header, then the blocks with each 16-bit word
// byte-swapped, as the 360 stores them
std::string Bitmap(uint8_t encoding, uint8_t bpp, uint16_t width, uint16_t height,
                   const std::vector<uint8_t>& blocks, uint8_t mips = 0) {
    std::string file(32, '\0');
    file[0] = 1;
    file[1] = static_cast<char>(bpp);
    file[2] = static_cast<char>(encoding);
    file[6] = static_cast<char>(mips);
    file[7] = static_cast<char>(width & 0xFF);
    file[8] = static_cast<char>(width >> 8);
    file[9] = static_cast<char>(height & 0xFF);
    file[10] = static_cast<char>(height >> 8);
    const uint16_t pitch = static_cast<uint16_t>(width * bpp / 8);
    file[11] = static_cast<char>(pitch & 0xFF);
    file[12] = static_cast<char>(pitch >> 8);
    for (size_t i = 0; i + 1 < blocks.size(); i += 2) {
        file += static_cast<char>(blocks[i + 1]);
        file += static_cast<char>(blocks[i]);
    }
    return file;
}

// a DXT1 block as a PC stores it: two little-endian RGB565 colors, then 2-bit
// indices, the first pixel in the lowest bits
std::vector<uint8_t> Dxt1Block(uint16_t c0, uint16_t c1, uint32_t indices) {
    return {static_cast<uint8_t>(c0), static_cast<uint8_t>(c0 >> 8),
            static_cast<uint8_t>(c1), static_cast<uint8_t>(c1 >> 8),
            static_cast<uint8_t>(indices), static_cast<uint8_t>(indices >> 8),
            static_cast<uint8_t>(indices >> 16), static_cast<uint8_t>(indices >> 24)};
}

std::array<uint8_t, 4> Pixel(const Image& image, int x, int y) {
    const size_t i = (static_cast<size_t>(y) * image.width + x) * 4;
    return {image.rgba[i], image.rgba[i + 1], image.rgba[i + 2], image.rgba[i + 3]};
}

constexpr uint16_t kRed = 0xF800;
constexpr uint16_t kBlue = 0x001F;
constexpr uint8_t kDxt1 = 8;
constexpr uint8_t kDxt5 = 24;

}

TEST_CASE("a DXT1 bitmap decodes its four colors") {
    // pixels 0-3 (the top row) take colors 0, 1, 2 and 3
    const Image image = *DecodeXboxBitmap(Bitmap(kDxt1, 4, 4, 4, Dxt1Block(kRed, kBlue, 0xE4)));
    REQUIRE(image.width == 4);
    REQUIRE(image.height == 4);
    CHECK(Pixel(image, 0, 0) == std::array<uint8_t, 4>{255, 0, 0, 255});
    CHECK(Pixel(image, 1, 0) == std::array<uint8_t, 4>{0, 0, 255, 255});
    // two thirds of the way from one to the other, each way
    CHECK(Pixel(image, 2, 0) == std::array<uint8_t, 4>{170, 0, 85, 255});
    CHECK(Pixel(image, 3, 0) == std::array<uint8_t, 4>{85, 0, 170, 255});
    // the other rows are index 0
    CHECK(Pixel(image, 3, 3) == std::array<uint8_t, 4>{255, 0, 0, 255});
}

TEST_CASE("a DXT1 block whose first color is lower has a halfway color and transparent black") {
    const Image image = *DecodeXboxBitmap(Bitmap(kDxt1, 4, 4, 4, Dxt1Block(kBlue, kRed, 0xE4)));
    CHECK(Pixel(image, 2, 0) == std::array<uint8_t, 4>{127, 0, 127, 255});
    CHECK(Pixel(image, 3, 0) == std::array<uint8_t, 4>{0, 0, 0, 0});
}

TEST_CASE("blocks are laid out left to right, then top to bottom") {
    std::vector<uint8_t> blocks;
    for (uint16_t color : {kRed, kBlue, uint16_t(0x07E0), uint16_t(0xFFFF)}) {
        const auto block = Dxt1Block(color, 0, 0);
        blocks.insert(blocks.end(), block.begin(), block.end());
    }
    const Image image = *DecodeXboxBitmap(Bitmap(kDxt1, 4, 8, 8, blocks));
    CHECK(Pixel(image, 0, 0) == std::array<uint8_t, 4>{255, 0, 0, 255});
    CHECK(Pixel(image, 7, 3) == std::array<uint8_t, 4>{0, 0, 255, 255});
    CHECK(Pixel(image, 0, 7) == std::array<uint8_t, 4>{0, 255, 0, 255});
    CHECK(Pixel(image, 7, 7) == std::array<uint8_t, 4>{255, 255, 255, 255});
}

TEST_CASE("a DXT5 bitmap takes its alpha from the alpha block") {
    // alpha 255 and 0 with six steps between; pixel 0 takes the first, pixel 1
    // the second, pixel 2 the first step (index 2: 6/7 of the way to the first)
    const uint64_t alpha_indices = 0b010'001'000;
    std::vector<uint8_t> block = {255, 0};
    for (int i = 0; i < 6; i++) block.push_back(static_cast<uint8_t>(alpha_indices >> (8 * i)));
    // pixel 3 takes color 3
    const auto color = Dxt1Block(kBlue, kRed, 0xC0);
    block.insert(block.end(), color.begin(), color.end());

    const Image image = *DecodeXboxBitmap(Bitmap(kDxt5, 8, 4, 4, block));
    CHECK(Pixel(image, 0, 0) == std::array<uint8_t, 4>{0, 0, 255, 255});
    CHECK(Pixel(image, 1, 0) == std::array<uint8_t, 4>{0, 0, 255, 0});
    CHECK(Pixel(image, 2, 0) == std::array<uint8_t, 4>{0, 0, 255, 218});
    // DXT5's colors are always the four-color kind, even with the first lower,
    // so color 3 is a blend, not DXT1's transparent black
    CHECK(Pixel(image, 3, 0) == std::array<uint8_t, 4>{170, 0, 85, 255});
}

TEST_CASE("the mipmaps after the first image are ignored") {
    auto blocks = Dxt1Block(kRed, kBlue, 0);
    const auto mip = Dxt1Block(kBlue, kBlue, 0);
    blocks.insert(blocks.end(), mip.begin(), mip.end());
    const Image image = *DecodeXboxBitmap(Bitmap(kDxt1, 4, 4, 4, blocks, 1));
    CHECK(Pixel(image, 0, 0) == std::array<uint8_t, 4>{255, 0, 0, 255});
}

TEST_CASE("bitmaps it can't decode give nothing") {
    const auto block = Dxt1Block(kRed, kBlue, 0);
    // too short for its size
    CHECK(!DecodeXboxBitmap(Bitmap(kDxt1, 4, 8, 8, block)));
    // no header
    CHECK(!DecodeXboxBitmap(std::string(16, '\0')));
    CHECK(!DecodeXboxBitmap(""));
    // an encoding it doesn't know (3 is uncompressed RGBA)
    CHECK(!DecodeXboxBitmap(Bitmap(3, 32, 4, 4, std::vector<uint8_t>(64))));
    // sizes that aren't whole blocks, or are none at all
    CHECK(!DecodeXboxBitmap(Bitmap(kDxt1, 4, 6, 4, std::vector<uint8_t>(16))));
    CHECK(!DecodeXboxBitmap(Bitmap(kDxt1, 4, 0, 4, {})));
}

TEST_CASE("the game's album art path becomes the file it builds") {
    CHECK(XboxBitmapPath("songs/rehab/rehab_keep.png") == "songs/rehab/gen/rehab_keep.png_xbox");
    CHECK(XboxBitmapPath("rehab_keep.png") == "gen/rehab_keep.png_xbox");
    CHECK(XboxBitmapPath("").empty());
}

TEST_CASE("a JPEG of the image has its size in the frame header") {
    Image image;
    image.width = 16;
    image.height = 8;
    image.rgba.assign(16 * 8 * 4, 200);
    const std::string jpeg = EncodeJpeg(image);
    REQUIRE(jpeg.size() > 4);
    CHECK(static_cast<uint8_t>(jpeg[0]) == 0xFF);
    CHECK(static_cast<uint8_t>(jpeg[1]) == 0xD8);
    CHECK(static_cast<uint8_t>(jpeg[jpeg.size() - 2]) == 0xFF);
    CHECK(static_cast<uint8_t>(jpeg[jpeg.size() - 1]) == 0xD9);

    // the baseline frame header: FFC0, length, precision, height, width
    const size_t sof = jpeg.find("\xFF\xC0");
    REQUIRE(sof != std::string::npos);
    const auto at = [&](size_t i) { return static_cast<uint8_t>(jpeg[sof + i]); };
    CHECK((at(5) << 8 | at(6)) == 8);
    CHECK((at(7) << 8 | at(8)) == 16);
}

TEST_CASE("a PNG of the image keeps its size and its alpha") {
    Image image;
    image.width = 2;
    image.height = 3;
    image.rgba.assign(2 * 3 * 4, 128);
    const std::string png = EncodePng(image);
    REQUIRE(png.size() > 33);
    CHECK(png.substr(1, 3) == "PNG");
    CHECK(png.substr(12, 4) == "IHDR");
    const auto at = [&](size_t i) { return static_cast<uint8_t>(png[i]); };
    CHECK((at(16) << 24 | at(17) << 16 | at(18) << 8 | at(19)) == 2);
    CHECK((at(20) << 24 | at(21) << 16 | at(22) << 8 | at(23)) == 3);
    CHECK(at(24) == 8);  // bits per channel
    CHECK(at(25) == 6);  // RGBA
}
