#pragma once
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

struct PPCContext;

// Reading a file as RB3 does, through its NewFile and the File it returns: from
// the ARK, a loose file that overrides it (src/Hooks/file.cpp) or mounted
// content alike. These call the game's own functions, so they run on the game
// thread only.

namespace band3::files {

// the whole file; nullopt when the game can't open it, or it's over max_size
std::optional<std::string> ReadAll(PPCContext& ctx, uint8_t* base, const std::string& path,
                                   size_t max_size);

}
