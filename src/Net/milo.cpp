#include "milo.h"
#include <charconv>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>

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
    // a CABEDEAF block taken as stored: those after it are too
    bool stored_unflagged = false;
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
        } else if (stored_unflagged) {
            inflated = std::string(block);
        } else {
            inflated = Inflate(block);
            if (!inflated && i == 0 && StartsMilo(block)) {
                inflated = std::string(block);
                stored_unflagged = true;
            }
        }
        if (!inflated) return std::nullopt;
        out += *inflated;
    }
    return out;
}

namespace {

// reads a milo's big-endian values; once anything runs past the end (or a
// name is too long to be one), ok stays false and every read gives 0
struct Reader {
    std::string_view data;
    size_t pos = 0;
    bool ok = true;

    bool Has(size_t n) const { return ok && data.size() - pos >= n; }
    uint32_t Be(int bytes) {
        if (!Has(static_cast<size_t>(bytes))) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for (int i = 0; i < bytes; i++) v = v << 8 | static_cast<uint8_t>(data[pos++]);
        return v;
    }
    uint8_t U8() { return static_cast<uint8_t>(Be(1)); }
    uint16_t U16() { return static_cast<uint16_t>(Be(2)); }
    uint32_t U32() { return Be(4); }
    float F32() {
        const uint32_t v = U32();
        float f;
        std::memcpy(&f, &v, sizeof(f));
        return f;
    }
    std::string_view Name() {
        const uint32_t n = U32();
        if (n > kMaxName || !Has(n)) {
            ok = false;
            return {};
        }
        const std::string_view s = data.substr(pos, n);
        pos += n;
        return s;
    }
    static constexpr uint32_t kMaxName = 512;
};

// bounds a font's counts: past these it isn't one
constexpr uint32_t kMaxChars = 4096;
constexpr uint32_t kMaxKerning = 65536;
constexpr uint32_t kMaxTexture = 4096;

// a Font's data read from its material's name on (FindFont)
std::optional<Font> ReadFont(std::string_view raw, size_t at) {
    Reader r{raw, at};
    const std::string_view mat = r.Name();
    if (!r.ok || !mat.ends_with(".mat")) return std::nullopt;
    Font font;
    font.cell_w = r.F32();
    font.cell_h = r.F32();
    if (!(font.cell_w > 0 && font.cell_w < kMaxTexture && font.cell_h > 0 && font.cell_h < kMaxTexture)) {
        return std::nullopt;
    }
    r.F32();  // deprecated size
    font.base_kerning = r.F32();
    const uint32_t count = r.U32();
    if (!r.ok || count > kMaxChars) return std::nullopt;
    std::vector<uint16_t> chars(count);
    for (uint16_t& c : chars) c = r.U16();
    if (r.U8()) {
        const uint32_t kerns = r.U32();
        if (!r.ok || kerns > kMaxKerning) return std::nullopt;
        for (uint32_t i = 0; i < kerns && r.ok; i++) {
            const uint32_t key = r.U32();
            font.kerning.push_back({static_cast<uint16_t>(key & 0xFFFF), static_cast<uint16_t>(key >> 16),
                                    r.F32()});
        }
    }
    r.Name();  // the texture's owner
    font.monospace = r.U8() != 0;
    r.U8();  // packed
    font.texture_w = r.U32();
    font.texture_h = r.U32();
    if (!r.ok || font.texture_w == 0 || font.texture_w > kMaxTexture || font.texture_h == 0 ||
        font.texture_h > kMaxTexture) {
        return std::nullopt;
    }
    r.F32();  // the texture's cell, which cell and texture give
    r.F32();
    const uint32_t infos = r.U32();
    if (!r.ok || infos > kMaxKerning) return std::nullopt;
    std::map<uint16_t, Glyph> by_code;
    for (uint32_t i = 0; i < infos && r.ok; i++) {
        Glyph g;
        g.code = r.U16();
        g.u = r.F32();
        g.v = r.F32();
        g.width = r.F32();
        g.advance = r.F32();
        by_code[g.code] = g;
    }
    r.Name();  // the next font
    if (!r.ok) return std::nullopt;
    for (uint16_t c : chars) {
        const auto it = by_code.find(c);
        if (it != by_code.end()) font.glyphs.push_back(it->second);
    }
    return font;
}

uint16_t Be16At(std::string_view s, size_t at) {
    return static_cast<uint16_t>(static_cast<uint8_t>(s[at]) << 8 | static_cast<uint8_t>(s[at + 1]));
}

std::string Number(float f) {
    char buf[32];
    const auto [end, ec] = std::to_chars(buf, buf + sizeof(buf), f);
    return ec == std::errc() ? std::string(buf, end) : std::string("0");
}

}

std::optional<Font> FindFont(std::string_view raw) {
    std::optional<Font> best;
    // each ".mat" a name ends with, and the length before that name
    for (size_t end = raw.find(".mat"); end != std::string_view::npos; end = raw.find(".mat", end + 1)) {
        const size_t name_end = end + 4;
        for (size_t start = name_end >= Reader::kMaxName + 4 ? name_end - Reader::kMaxName - 4 : 0;
             start + 4 <= name_end; start++) {
            Reader r{raw, start};
            if (r.U32() != name_end - start - 4) continue;
            if (auto font = ReadFont(raw, start)) {
                if (!best || font->glyphs.size() > best->glyphs.size()) best = std::move(font);
            }
            break;
        }
    }
    return best;
}

std::optional<http::Image> FindBitmap(std::string_view raw) {
    for (size_t at = 0; at + 32 <= raw.size(); at++) {
        if (raw[at] != '\x01') continue;
        const uint8_t bpp = static_cast<uint8_t>(raw[at + 1]);
        Reader r{raw, at + 2};
        const uint32_t encoding = r.U32();
        if ((bpp != 4 && bpp != 8) || (encoding != 8 && encoding != 24)) continue;
        const uint16_t width = Be16At(raw, at + 7), height = Be16At(raw, at + 9);
        if (width < 4 || height < 4 || width > kMaxTexture || height > kMaxTexture) continue;
        if (raw.substr(at + 13, 19).find_first_not_of('\0') != std::string_view::npos) continue;
        // album_art's little-endian header, then the same blocks
        std::string file(32, '\0');
        file[0] = 1;
        file[1] = static_cast<char>(bpp);
        file[2] = static_cast<char>(encoding);
        file[6] = raw[at + 6];
        file[7] = static_cast<char>(width & 0xFF);
        file[8] = static_cast<char>(width >> 8);
        file[9] = static_cast<char>(height & 0xFF);
        file[10] = static_cast<char>(height >> 8);
        file[11] = raw[at + 12];
        file[12] = raw[at + 11];
        file += raw.substr(at + 32);
        if (auto image = http::DecodeXboxBitmap(file)) return image;
    }
    return std::nullopt;
}

std::string FormatFontJson(const Font& font) {
    std::string out = "{\"cell\":[" + Number(font.cell_w) + "," + Number(font.cell_h) +
                      "],\"texture\":[" + std::to_string(font.texture_w) + "," +
                      std::to_string(font.texture_h) + "],\"base_kerning\":" +
                      Number(font.base_kerning) + ",\"monospace\":" +
                      (font.monospace ? "true" : "false") + ",\"glyphs\":{";
    for (size_t i = 0; i < font.glyphs.size(); i++) {
        const Glyph& g = font.glyphs[i];
        if (i) out += ',';
        out += "\"" + std::to_string(g.code) + "\":[" + Number(g.u) + "," + Number(g.v) + "," +
               Number(g.width) + "," + Number(g.advance) + "]";
    }
    out += "},\"kerning\":[";
    for (size_t i = 0; i < font.kerning.size(); i++) {
        const Kern& k = font.kerning[i];
        if (i) out += ',';
        out += "[" + std::to_string(k.left) + "," + std::to_string(k.right) + "," + Number(k.kerning) + "]";
    }
    return out + "]}";
}

}
