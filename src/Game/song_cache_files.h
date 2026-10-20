#pragma once
#include <filesystem>
#include <string_view>
#include <vector>

// The song cache's files in the user data root, apart from the SDK so the unit
// tests can check them (tests/song_cache_files_test.cpp).

namespace band3::song_cache {

// songcache, or Rock Band 3 Deluxe's rbdxcache
bool IsSongCache(std::string_view file_name);

// Writes the empty cache a new profile starts with into the package `file_name`
// of `content_type` (8 hex digits, as the SDK names its folders), in every
// profile folder under `user_data_root` that has the package but not its cache
// file. The game fails to read such a package and never writes a cache to it
// again, so every start would scan the whole library; reading the empty cache,
// it scans once and writes the full one. Returns the files it wrote.
std::vector<std::filesystem::path> RestoreMissingCaches(const std::filesystem::path& user_data_root,
                                                        std::string_view title_id,
                                                        std::string_view content_type,
                                                        std::string_view file_name);

}
