#pragma once
#include <optional>
#include <string>
#include <string_view>

// RB3's .milo_xbox files, for the web server's game assets (http_server.h):
// decompressed, and the fonts and textures in them read. Nothing here needs
// the game.

namespace band3::milo {

// The objects' stream a .milo_xbox holds. Its header (little-endian): magic,
// data offset, block count, largest block, then each block's size, 0x01000000
// marking one stored. 0xCDBEDEAF's compressed blocks are a 4-byte size, then
// raw deflate; 0xCABEDEAF's are raw deflate, though Deluxe writes some stored
// without the flag (they start with the milo's version). nullopt for another
// magic, a block past the end, or one that neither inflates nor is stored.
std::optional<std::string> Decompress(std::string_view file);

}
