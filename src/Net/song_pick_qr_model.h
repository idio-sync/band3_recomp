#pragma once
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "src/Net/album_art.h"
#include "src/Net/milo.h"

// The song list's QR code for the web server's page (song_pick_qr.h), apart
// from ImGui and the game so the unit tests reach it: the page's address, the
// code's modules, where it all goes, and the caption set in one of RB3's
// bitmap fonts (milo.h's Font) the way /karaoke sets its lyrics.

namespace band3::song_pick_qr {

// The page's address for a phone on the local network, from the address the
// server listens on (http_address) and this PC's (local_address.h): the
// local one for 0.0.0.0, else the one it listens on. Empty when a phone
// couldn't reach it: a loopback address, or no local one to give.
std::string PageUrl(std::string_view listen_address, std::string_view local_address, int port);

// the address as the caption shows it: no scheme, no trailing slash, in
// capitals as the menu's text is ("http://192.168.1.42:21070/" to
// "192.168.1.42:21070")
std::string Caption(std::string_view url);

// A QR code's modules, `quiet` light ones around it: size × size, row by row,
// true dark. Medium error correction, the smallest version that holds the
// text. nullopt for text too long for one.
struct Modules {
    int size = 0;
    std::vector<bool> dark;
    bool Dark(int x, int y) const { return dark[size_t(y) * size_t(size) + size_t(x)]; }
};
std::optional<Modules> Encode(const std::string& text, int quiet);
inline constexpr int kQuiet = 2;

// Where it goes, in the game's 1280x720 frame, as the mockup has it: the code
// in the gap between "Music Library" and the profile's name, its bottom level
// with the title's, and two lines of caption beside it, their bottom level
// with the code's. The lines are each this tall (the font's cell), in the
// greys of the profile's name and the "viewing all ... songs" bar.
inline constexpr float kFrameWidth = 1280.0f, kFrameHeight = 720.0f;
inline constexpr float kCodeLeft = 545.0f, kCodeBottom = 72.0f, kCodeSize = 60.0f;
inline constexpr float kCaptionGap = 10.0f;  // the code to the caption
inline constexpr float kLineGap = 2.0f;      // the first line to the second
inline constexpr float kTitlePx = 15.0f, kAddressPx = 13.0f;
inline constexpr uint8_t kTitleGrey = 193, kAddressGrey = 169;
inline constexpr char kTitle[] = "SCAN TO PICK SONGS";

// where the game's 1280x720 frame lies in the window, in its pixels: the
// largest centred 16:9 rectangle with `letterbox` (the picture letterboxed,
// or the game's cameras filling a wider window, whose menus keep to the
// middle 16:9), else the whole window (the picture stretched)
struct Rect {
    float x = 0, y = 0, w = 0, h = 0;
};
Rect GameFrame(uint32_t window_w, uint32_t window_h, bool letterbox);

// The code's pixels per module at a frame scale (window pixels per frame
// pixel): whole pixels, so every module's edge is sharp, as near kCodeSize
// across as that allows, and at least 1
int ModulePixels(int modules, float scale);

// Text set in a font `px` tall (its cell's height): each character's glyph,
// where it is in the texture (pixels) and where it goes along the line. A
// character the font lacks moves the line on by a third of its height.
struct Placed {
    float src_x = 0, src_y = 0, src_w = 0, src_h = 0;
    float x = 0, w = 0;
};
struct Line {
    std::vector<Placed> glyphs;
    float width = 0;
};
Line Layout(const milo::Font& font, std::string_view text, float px, int texture_w, int texture_h);

// A laid-out line drawn into an alpha mask `height` pixels tall and as wide
// as the line (rounded up), each pixel the average of the texture's alpha
// over the part of the glyph it covers, so a font drawn far bigger than this
// shrinks without shimmering.
struct Mask {
    int width = 0, height = 0;
    std::vector<uint8_t> alpha;
};
Mask Rasterize(const Line& line, const http::Image& texture, int height);

}  // namespace band3::song_pick_qr
