#include "src/Render/png_writer.h"

#include <cstdio>
#include <string>

// See png_writer.h.

namespace band3::render {
namespace {


// ---------------------------------------------------------------------------
// PNG, stored (uncompressed) deflate, so no library is needed

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = table[(crc ^ p[i]) & 0xff] ^ (crc >> 8);
    return ~crc;
}

void PutBe32(std::string& s, uint32_t v) {
    for (int i = 3; i >= 0; i--) s.push_back(char((v >> (8 * i)) & 0xff));
}

void Chunk(std::string& out, const char* type, const std::string& data) {
    PutBe32(out, uint32_t(data.size()));
    std::string body(type, 4);
    body += data;
    out += body;
    PutBe32(out, Crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
}

}  // namespace

bool WritePng(const std::string& path, const std::vector<uint32_t>& rgba, uint32_t w,
              uint32_t h) {
    std::string raw;
    raw.reserve(size_t(h) * (w * 4 + 1));
    for (uint32_t y = 0; y < h; y++) {
        raw.push_back(0);
        raw.append(reinterpret_cast<const char*>(&rgba[size_t(y) * w]), size_t(w) * 4);
    }
    std::string z;
    z.push_back(0x78);
    z.push_back(0x01);
    for (size_t pos = 0; pos < raw.size() || pos == 0;) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(char(n & 0xff));
        z.push_back(char(n >> 8));
        z.push_back(char(~n & 0xff));
        z.push_back(char((~n >> 8) & 0xff));
        z.append(raw, pos, n);
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (unsigned char c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    PutBe32(z, (b << 16) | a);

    std::string png("\x89PNG\r\n\x1a\n", 8);
    std::string ihdr;
    PutBe32(ihdr, w);
    PutBe32(ihdr, h);
    ihdr += std::string("\x08\x06\x00\x00\x00", 5);
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", z);
    Chunk(png, "IEND", "");

    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    std::fwrite(png.data(), 1, png.size(), f);
    std::fclose(f);
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
}

}  // namespace band3::render
