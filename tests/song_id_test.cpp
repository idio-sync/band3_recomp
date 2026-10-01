// Checks the song ID a custom song with a text song_id gets
// (src/Game/song_id.cpp): RB3Enhanced's, so the two agree.

#include <doctest/doctest.h>
#include "src/Game/song_id.h"

TEST_CASE("CRC-32 is the standard one") {
    CHECK(band3::Crc32("123456789") == 0xCBF43926u);
    CHECK(band3::Crc32("") == 0u);
}

TEST_CASE("text song IDs become RB3Enhanced's numbers") {
    CHECK(band3::CorrectedSongId("KMFDMMega") == 2133239510);
    CHECK(band3::CorrectedSongId("dust_pc") == 2133960588);
    CHECK(band3::CorrectedSongId("mardigras_pc") == 2138512288);
    CHECK(band3::CorrectedSongId("123456789") == 2131780604);
}
