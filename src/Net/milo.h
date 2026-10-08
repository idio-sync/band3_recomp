#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/Net/album_art.h"

// RB3's .milo_xbox files, for the web server's game assets (http_server.h):
// decompressed, and the fonts and textures in them read. Nothing here needs
// the game.

namespace band3::milo {

// The objects' stream a .milo_xbox holds. Its header (little-endian): magic,
// data offset, block count, largest block, then each block's size, 0x01000000
// marking one stored. 0xCDBEDEAF's compressed blocks are a 4-byte size, then
// raw deflate; 0xCABEDEAF's are raw deflate, though Deluxe writes some files
// stored without the flag (the first block, then, starts with the milo's
// version, and the rest follow it). nullopt for another magic, a block past
// the end, or one that neither inflates nor is stored.
std::optional<std::string> Decompress(std::string_view file);

// One character of a font (RndFont's CharInfo): where its glyph is in the
// texture (u, v, 0 to 1), its width and its advance (in cell widths). Its
// rectangle in the texture: u × the texture's width, v × its height, width ×
// the cell's width across, the cell's height down.
struct Glyph {
    uint16_t code = 0;
    float u = 0, v = 0, width = 0, advance = 0;
};

// the kerning between a pair of characters, in cell widths
struct Kern {
    uint16_t left = 0, right = 0;
    float kerning = 0;
};

// What a page needs of an RB3 font (RndFont, revision 17) to draw text from
// its texture
struct Font {
    float cell_w = 0, cell_h = 0;  // pixels in the texture
    float base_kerning = 0;        // cell widths, between every pair
    uint32_t texture_w = 0, texture_h = 0;
    bool monospace = false;
    std::vector<Glyph> glyphs;  // the characters it lists, those it has glyphs for
    std::vector<Kern> kerning;
};

// The font in a decompressed milo: of the Font objects that read whole, the
// one with the most glyphs (the first of those with as many). A Font's data
// from its material's name on (RndFont::Load, big-endian): the material
// (u32 length + name, ending ".mat"), f32 cell width and height, f32
// deprecated size, f32 base kerning, u32 count + u16 characters, u8 kerning
// table [u32 count + (u32 key: low 16 bits the left character, high the
// right; f32 kerning)], the texture owner (a name), u8 monospace, u8 packed,
// u32 texture width and height, f32 texture cell width and height, u32
// count + (u16 character, f32 u, v, width, advance), the next font (a name).
// nullopt when none reads whole.
std::optional<Font> FindFont(std::string_view raw);

// The first DXT1 or DXT5 bitmap in a decompressed milo, decoded: RndBitmap,
// as a .png_xbox stores it (album_art.h) but with its header big-endian: u8
// version 1, u8 bits per pixel, u32 encoding (8 DXT1, 24 DXT5), u8 mips, u16
// width, u16 height, u16 bytes per line, 19 zero bytes, then the blocks.
std::optional<http::Image> FindBitmap(std::string_view raw);

// /game_asset/font's JSON: {"cell":[w,h],"texture":[w,h],"base_kerning":k,
// "monospace":b,"glyphs":{"<code>":[u,v,width,advance],...},"kerning":[[left,right,k],...]}
std::string FormatFontJson(const Font& font);

}
