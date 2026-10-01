#pragma once
#include <filesystem>

// RB3's song cache: the content package (songcache, or Rock Band 3 Deluxe's
// rbdxcache) the game keeps its scanned song list in, which RB3Enhanced's
// rb3e_delete_songcache deletes. The game has it open while it runs, so band3
// deletes it the next time it starts, before the game mounts anything; Deluxe
// relaunches straight after asking, as it does on RB3E.

namespace band3::song_cache {

// Marks the song cache the game mounted for deletion at the next start. False
// when the game hasn't mounted one, or its folder can't be found.
bool RequestDelete();

// Deletes what RequestDelete marked. Call once, before the game starts.
void DeletePending(const std::filesystem::path& user_data_root);

}
