#include "src/Render/png_writer.h"

#include <cstdio>
#include <cstring>
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

namespace {

uint32_t Be32(const std::string& s, size_t at) {
    return (uint32_t(uint8_t(s[at])) << 24) | (uint32_t(uint8_t(s[at + 1])) << 16) |
           (uint32_t(uint8_t(s[at + 2])) << 8) | uint32_t(uint8_t(s[at + 3]));
}

}  // namespace

bool ReadPng(const std::string& path, std::vector<uint32_t>& rgba, uint32_t& w, uint32_t& h) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    std::string data;
    char buf[1 << 16];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) data.append(buf, n);
    std::fclose(f);
    if (data.size() < 8 || data.compare(0, 8, std::string("\x89PNG\r\n\x1a\n", 8)) != 0)
        return false;

    std::string z;
    w = h = 0;
    for (size_t at = 8; at + 12 <= data.size();) {
        const uint32_t len = Be32(data, at);
        const std::string type = data.substr(at + 4, 4);
        if (at + 12 + len > data.size()) return false;
        if (type == "IHDR") {
            w = Be32(data, at + 8);
            h = Be32(data, at + 12);
            // 8-bit RGBA, no interlace
            if (data[at + 16] != 8 || data[at + 17] != 6 || data[at + 20] != 0) return false;
        } else if (type == "IDAT") {
            z.append(data, at + 8, len);
        }
        at += 12 + len;
    }
    if (!w || !h || z.size() < 2) return false;

    // stored deflate blocks only
    std::string raw;
    for (size_t at = 2; at + 5 <= z.size();) {
        const bool last = z[at] & 1;
        if ((uint8_t(z[at]) >> 1) & 3) return false;
        const size_t len = uint8_t(z[at + 1]) | (size_t(uint8_t(z[at + 2])) << 8);
        if (at + 5 + len > z.size()) return false;
        raw.append(z, at + 5, len);
        at += 5 + len;
        if (last) break;
    }
    const size_t row = size_t(w) * 4 + 1;
    if (raw.size() < row * h) return false;
    rgba.resize(size_t(w) * h);
    for (uint32_t y = 0; y < h; y++) {
        if (raw[y * row] != 0) return false;  // filter none only
        std::memcpy(&rgba[size_t(y) * w], raw.data() + y * row + 1, size_t(w) * 4);
    }
    return true;
}

}  // namespace band3::render
