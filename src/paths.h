#pragma once
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

// band3's folder settings: where its folders are, and the folders it reads
// songs from. A relative path is relative to the folder band3_config.ini is
// in, so a whole install can move as one folder.

namespace band3::paths {

// value (UTF-8) resolved against anchor; an empty value stays empty
std::filesystem::path Resolve(std::string_view value, const std::filesystem::path& anchor);

// the first dir / name that is a file, or empty
std::filesystem::path FindFile(const std::vector<std::filesystem::path>& dirs,
                               std::string_view name);

// "a| b ||c" -> {"a", "b", "c"}
std::vector<std::string> SplitList(std::string_view value);

}
