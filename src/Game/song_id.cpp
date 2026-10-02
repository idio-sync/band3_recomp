#include "src/Game/song_id.h"

#include <array>

namespace band3 {

namespace {

std::array<uint32_t, 256> MakeCrcTable() {
    std::array<uint32_t, 256> table{};
    for (uint32_t i = 0; i < 256; ++i) {
        uint32_t c = i;
        for (int k = 0; k < 8; ++k)
            c = (c & 1) ? (c >> 1) ^ 0xEDB88320u : c >> 1;
        table[i] = c;
    }
    return table;
}

}  // namespace

uint32_t Crc32(std::string_view text) {
    static const std::array<uint32_t, 256> table = MakeCrcTable();
    uint32_t crc = ~0u;
    for (char ch : text)
        crc = table[(crc ^ static_cast<uint8_t>(ch)) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

int32_t CorrectedSongId(std::string_view text) {
    return static_cast<int32_t>(Crc32(text) % 9999999 + 2130000000);
}

}
