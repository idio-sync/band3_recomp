#pragma once

#include <cstdint>
#include <string_view>

namespace band3 {

// standard CRC-32 (zlib's)
uint32_t Crc32(std::string_view text);

// the ID RB3Enhanced gives a song whose song_id is text
int32_t CorrectedSongId(std::string_view text);

// logs a correction once per distinct text (src/Hooks/song_id.cpp)
void LogSongIdCorrection(std::string_view text, int32_t id);

}
