#include "album_art.h"

#include <array>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_STATIC
#define STBI_WRITE_NO_STDIO
#include "src/ThirdParty/stb/stb_image_write.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace band3::http {

namespace {

// RndBitmap's header, little-endian; the rest of its 32 bytes are zero
constexpr size_t kHeaderSize = 32;
constexpr size_t kEncodingOffset = 2;  // u32
constexpr size_t kWidthOffset = 7;     // u16
constexpr size_t kHeightOffset = 9;    // u16

constexpr uint32_t kDxt1 = 8;
constexpr uint32_t kDxt5 = 24;

// album art is 256x256; this keeps a broken header from asking for gigabytes
constexpr int kMaxSide = 4096;

// covers are photos, already DXT-compressed: 85 keeps them looking as they do
// in the game at a tenth of a PNG's size
constexpr int kJpegQuality = 85;

using Rgba = std::array<uint8_t, 4>;

uint32_t Le16(std::string_view file, size_t at) {
    return static_cast<uint8_t>(file[at]) | static_cast<uint8_t>(file[at + 1]) << 8;
}

uint32_t Le32(std::string_view file, size_t at) {
    return Le16(file, at) | Le16(file, at + 2) << 16;
}

// the block's bytes as a PC has them: the 360 swaps each 16-bit word
std::array<uint8_t, 16> Unswap(std::string_view file, size_t at, size_t size) {
    std::array<uint8_t, 16> block{};
    for (size_t i = 0; i < size; i += 2) {
        block[i] = static_cast<uint8_t>(file[at + i + 1]);
        block[i + 1] = static_cast<uint8_t>(file[at + i]);
    }
    return block;
}

Rgba From565(uint32_t c) {
    const uint32_t r = c >> 11, g = (c >> 5) & 63, b = c & 31;
    return {static_cast<uint8_t>(r * 255 / 31), static_cast<uint8_t>(g * 255 / 63),
            static_cast<uint8_t>(b * 255 / 31), 255};
}

Rgba Mix(const Rgba& a, const Rgba& b, uint32_t wa, uint32_t wb) {
    Rgba out{};
    for (int i = 0; i < 3; i++) out[i] = static_cast<uint8_t>((a[i] * wa + b[i] * wb) / (wa + wb));
    out[3] = 255;
    return out;
}

// a DXT1 color block (8 bytes at `color`) into 16 pixels; DXT5's is always the
// four-color kind
void DecodeColors(const uint8_t* color, bool four_always, std::array<Rgba, 16>& pixels) {
    const uint32_t c0 = color[0] | color[1] << 8;
    const uint32_t c1 = color[2] | color[3] << 8;
    const Rgba a = From565(c0), b = From565(c1);
    std::array<Rgba, 4> palette;
    if (c0 > c1 || four_always) {
        palette = {a, b, Mix(a, b, 2, 1), Mix(a, b, 1, 2)};
    } else {
        palette = {a, b, Mix(a, b, 1, 1), Rgba{0, 0, 0, 0}};
    }
    const uint32_t indices = color[4] | color[5] << 8 | color[6] << 16 |
                             static_cast<uint32_t>(color[7]) << 24;
    for (int i = 0; i < 16; i++) pixels[i] = palette[(indices >> (2 * i)) & 3];
}

// a DXT5 alpha block (8 bytes) into the 16 pixels' alpha
void DecodeAlpha(const uint8_t* alpha, std::array<Rgba, 16>& pixels) {
    const uint32_t a0 = alpha[0], a1 = alpha[1];
    std::array<uint32_t, 8> palette{a0, a1};
    if (a0 > a1) {
        for (uint32_t i = 1; i < 7; i++) palette[i + 1] = ((7 - i) * a0 + i * a1) / 7;
    } else {
        for (uint32_t i = 1; i < 5; i++) palette[i + 1] = ((5 - i) * a0 + i * a1) / 5;
        palette[6] = 0;
        palette[7] = 255;
    }
    uint64_t indices = 0;
    for (int i = 0; i < 6; i++) indices |= static_cast<uint64_t>(alpha[2 + i]) << (8 * i);
    for (int i = 0; i < 16; i++) {
        pixels[i][3] = static_cast<uint8_t>(palette[(indices >> (3 * i)) & 7]);
    }
}

void AppendBytes(void* context, void* data, int size) {
    static_cast<std::string*>(context)->append(static_cast<const char*>(data),
                                               static_cast<size_t>(size));
}

}

std::string XboxBitmapPath(std::string_view path) {
    if (path.empty()) return {};
    const size_t slash = path.rfind('/');
    const size_t name = slash == std::string_view::npos ? 0 : slash + 1;
    std::string out(path.substr(0, name));
    out += "gen/";
    out += path.substr(name);
    out += "_xbox";
    return out;
}

std::optional<Image> DecodeXboxBitmap(std::string_view file) {
    if (file.size() < kHeaderSize) return std::nullopt;
    const uint32_t encoding = Le32(file, kEncodingOffset);
    const int width = static_cast<int>(Le16(file, kWidthOffset));
    const int height = static_cast<int>(Le16(file, kHeightOffset));
    if (encoding != kDxt1 && encoding != kDxt5) return std::nullopt;
    if (width <= 0 || height <= 0 || width > kMaxSide || height > kMaxSide ||
        width % 4 || height % 4) {
        return std::nullopt;
    }
    const size_t block_size = encoding == kDxt1 ? 8 : 16;
    const int blocks_x = width / 4, blocks_y = height / 4;
    if (file.size() < kHeaderSize + block_size * blocks_x * blocks_y) return std::nullopt;

    Image image;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<size_t>(width) * height * 4);
    size_t at = kHeaderSize;
    std::array<Rgba, 16> pixels;
    for (int by = 0; by < blocks_y; by++) {
        for (int bx = 0; bx < blocks_x; bx++, at += block_size) {
            const auto block = Unswap(file, at, block_size);
            if (encoding == kDxt1) {
                DecodeColors(block.data(), false, pixels);
            } else {
                DecodeColors(block.data() + 8, true, pixels);
                DecodeAlpha(block.data(), pixels);
            }
            for (int i = 0; i < 16; i++) {
                const size_t x = bx * 4 + i % 4, y = by * 4 + i / 4;
                const size_t out = (y * width + x) * 4;
                for (int c = 0; c < 4; c++) image.rgba[out + c] = pixels[i][c];
            }
        }
    }
    return image;
}

std::string EncodeJpeg(const Image& image) {
    std::string jpeg;
    stbi_write_jpg_to_func(AppendBytes, &jpeg, image.width, image.height, 4, image.rgba.data(),
                           kJpegQuality);
    return jpeg;
}

std::string EncodePng(const Image& image) {
    std::string png;
    stbi_write_png_to_func(AppendBytes, &png, image.width, image.height, 4, image.rgba.data(),
                           image.width * 4);
    return png;
}

}
