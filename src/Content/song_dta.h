#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// A package's songs/songs.dta, the text the game reads its songs from, as far
// as telling songs apart needs: each top-level entry's shortname, song_id,
// name and artist. DTA is lists in (), [] or {} of numbers, symbols ('quoted'
// or bare) and "strings", with ; and /* */ comments and # directives.

namespace band3::content {

struct DtaSong {
    std::string shortname;
    // as the game has it: a text song_id as band3 corrects it (CorrectedSongId);
    // 0 for none
    int32_t song_id = 0;
    std::string title;   // its name, as UTF-8
    std::string artist;  // as UTF-8
};

// the songs in a songs.dta, in its order; text that isn't DTA, and entries
// that aren't songs, are left out
std::vector<DtaSong> ParseSongsDta(std::string_view text);

}
