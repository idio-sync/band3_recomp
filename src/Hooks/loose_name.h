// the name in game:\ of a loose file that replaces an ARK file (see file.cpp)
#pragma once
#include <cstddef>
#include <string>
#include <string_view>

namespace band3 {

// The longest name a loose file can have. The game opens one as d:\<name>
// through 256-byte path buffers, so a longer one would overflow them; it's
// read from the ARK instead, as RB3Enhanced does.
inline constexpr size_t kMaxLooseName = 250;

// the loose file's name for an ARK path: each ".." folder is a folder named
// "(..)", as RB3Enhanced lays them out, since game:\ can't go above itself;
// folders are separated by '/'
inline std::string LooseName(std::string_view ark_path) {
    std::string out;
    size_t start = 0;
    while (start <= ark_path.size()) {
        size_t end = ark_path.find_first_of("/\\", start);
        if (end == std::string_view::npos) end = ark_path.size();
        const std::string_view part = ark_path.substr(start, end - start);
        out += part == ".." ? std::string_view("(..)") : part;
        if (end < ark_path.size()) out += '/';
        start = end + 1;
    }
    return out;
}

}  // namespace band3
