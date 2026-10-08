#include "milo.h"
#include <cstdint>
#include <cstdlib>

#if defined(_MSC_VER)
#pragma warning(push, 0)
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wall"
#pragma GCC diagnostic ignored "-Wextra"
#endif
// for its zlib inflate only
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#include "src/ThirdParty/stb/stb_image.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

namespace band3::milo {

namespace {

constexpr uint32_t kCompressedSized = 0xCDBEDEAF;  // each block: its size, then raw deflate
constexpr uint32_t kCompressed = 0xCABEDEAF;       // raw deflate
constexpr uint32_t kStoredFlag = 0x01000000;
constexpr uint32_t kSizeMask = 0x00FFFFFF;

uint32_t Le32(std::string_view s, size_t at) {
    return static_cast<uint32_t>(static_cast<uint8_t>(s[at])) |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 1])) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 2])) << 16 |
           static_cast<uint32_t>(static_cast<uint8_t>(s[at + 3])) << 24;
}

std::optional<std::string> Inflate(std::string_view raw) {
    int size = 0;
    char* out = stbi_zlib_decode_noheader_malloc(raw.data(), static_cast<int>(raw.size()), &size);
    if (!out) return std::nullopt;
    std::string result(out, static_cast<size_t>(size));
    STBI_FREE(out);
    return result;
}

// a milo's objects start with its version, big-endian (RB3's are 0x1c)
bool StartsMilo(std::string_view block) {
    if (block.size() < 4) return false;
    const uint32_t version = static_cast<uint32_t>(static_cast<uint8_t>(block[0])) << 24 |
                             static_cast<uint32_t>(static_cast<uint8_t>(block[1])) << 16 |
                             static_cast<uint32_t>(static_cast<uint8_t>(block[2])) << 8 |
                             static_cast<uint8_t>(block[3]);
    return version >= 0x10 && version <= 0x30;
}

}

std::optional<std::string> Decompress(std::string_view file) {
    if (file.size() < 16) return std::nullopt;
    const uint32_t magic = Le32(file, 0);
    if (magic != kCompressedSized && magic != kCompressed) return std::nullopt;
    const uint32_t offset = Le32(file, 4);
    const uint32_t blocks = Le32(file, 8);
    if (offset > file.size() || blocks > (offset - 16) / 4) return std::nullopt;
    std::string out;
    size_t at = offset;
    for (uint32_t i = 0; i < blocks; i++) {
        const uint32_t entry = Le32(file, 16 + 4 * static_cast<size_t>(i));
        const size_t size = entry & kSizeMask;
        if (size > file.size() - at) return std::nullopt;
        const std::string_view block = file.substr(at, size);
        at += size;
        if (entry & kStoredFlag) {
            out += block;
            continue;
        }
        std::optional<std::string> inflated;
        if (magic == kCompressedSized) {
            if (block.size() >= 4) inflated = Inflate(block.substr(4));
        } else {
            inflated = Inflate(block);
            if (!inflated && StartsMilo(block)) inflated = std::string(block);
        }
        if (!inflated) return std::nullopt;
        out += *inflated;
    }
    return out;
}

}
