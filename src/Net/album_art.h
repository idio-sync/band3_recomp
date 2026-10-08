#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Album art for the web server's /album_art (http_server.h): a song's
// <shortname>_keep.png_xbox, which the game reads for the Music Library,
// decoded and sent as a JPEG. Nothing here needs the game.

namespace band3::http {

struct Image {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // width * height pixels, top row first
};

// the file the game builds a texture path into: songs/x/x_keep.png is
// songs/x/gen/x_keep.png_xbox. Empty for an empty path.
std::string XboxBitmapPath(std::string_view path);

// RB3's Xbox 360 bitmap (RndBitmap): a 32-byte header, then the image and its
// mipmaps, DXT1 (encoding 8) or DXT5 (24) in rows of 4x4 blocks, with every
// 16-bit word byte-swapped. The first image only; nullopt for any other
// encoding or a file too short for its size.
std::optional<Image> DecodeXboxBitmap(std::string_view file);

std::string EncodeJpeg(const Image& image);

// a PNG of it, alpha kept, for the game assets (milo.h)
std::string EncodePng(const Image& image);

}
