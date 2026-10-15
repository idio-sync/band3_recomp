// Checks src/Net/song_pick_qr_model.h: the page's address the song list's QR
// code gives, the code's modules, where the game's frame lies in the window,
// and the caption laid out and drawn from a font's texture.

#include <doctest/doctest.h>
#include "src/Net/song_pick_qr_model.h"

using namespace band3::song_pick_qr;

namespace {

// a font of two 10x20 cells side by side in a 20x20 texture: 'A' fully
// opaque, 'B' clear, both a cell wide, advancing a cell; A then B kerned
// in by a tenth of a cell
band3::milo::Font TwoCellFont() {
    band3::milo::Font f;
    f.cell_w = 10;
    f.cell_h = 20;
    f.texture_w = 20;
    f.texture_h = 20;
    f.glyphs = {{'A', 0.0f, 0.0f, 1.0f, 1.0f}, {'B', 0.5f, 0.0f, 1.0f, 1.0f}};
    f.kerning = {{'A', 'B', -0.1f}};
    return f;
}

band3::http::Image TwoCellTexture() {
    band3::http::Image image;
    image.width = 20;
    image.height = 20;
    image.rgba.assign(20 * 20 * 4, 255);
    for (int y = 0; y < 20; y++)
        for (int x = 10; x < 20; x++) image.rgba[(y * 20 + x) * 4 + 3] = 0;
    return image;
}

}  // namespace

TEST_CASE("the page's address: this PC's on the network, or the one the server listens on") {
    CHECK(PageUrl("0.0.0.0", "192.168.1.42", 21070) == "http://192.168.1.42:21070/");
    CHECK(PageUrl("10.0.0.5", "192.168.1.42", 8080) == "http://10.0.0.5:8080/");
    // a phone can't reach these
    CHECK(PageUrl("127.0.0.1", "192.168.1.42", 21070).empty());
    CHECK(PageUrl("0.0.0.0", "", 21070).empty());
    CHECK(PageUrl("0.0.0.0", "127.0.0.1", 21070).empty());
}

TEST_CASE("the caption's address drops the scheme and the slash, in capitals") {
    CHECK(Caption("http://192.168.1.42:21070/") == "192.168.1.42:21070");
    CHECK(Caption("http://band3.local:21070/") == "BAND3.LOCAL:21070");
    CHECK(Caption("192.168.1.42") == "192.168.1.42");
}

TEST_CASE("the code: version 2 for a LAN address, with its quiet zone light") {
    const auto m = Encode("http://192.168.1.42:21070/", kQuiet);
    REQUIRE(m);
    // version 2 is 25 modules across
    CHECK(m->size == 25 + 2 * kQuiet);
    for (int i = 0; i < m->size; i++) {
        CHECK_FALSE(m->Dark(i, 0));
        CHECK_FALSE(m->Dark(0, i));
        CHECK_FALSE(m->Dark(i, m->size - 1));
        CHECK_FALSE(m->Dark(m->size - 1, i));
    }
    // the finder pattern's dark corner, just inside the quiet zone
    CHECK(m->Dark(kQuiet, kQuiet));
    CHECK_FALSE(m->Dark(kQuiet + 1, kQuiet + 1));
    CHECK(m->Dark(kQuiet + 2, kQuiet + 2));
}

TEST_CASE("the code: nothing for text no QR code holds") {
    CHECK_FALSE(Encode(std::string(8000, 'x'), kQuiet));
}

TEST_CASE("the game's frame: centred 16:9 when letterboxed, the window when stretched") {
    const Rect wide = GameFrame(2560, 1080, true);
    CHECK(wide.x == 320);
    CHECK(wide.y == 0);
    CHECK(wide.w == 1920);
    CHECK(wide.h == 1080);
    const Rect tall = GameFrame(1920, 1200, true);
    CHECK(tall.x == 0);
    CHECK(tall.y == 60);
    CHECK(tall.w == 1920);
    CHECK(tall.h == 1080);
    const Rect stretched = GameFrame(2560, 1080, false);
    CHECK(stretched.w == 2560);
    CHECK(stretched.h == 1080);
}

TEST_CASE("modules are whole pixels, as near the code's size as that allows") {
    // 29 modules in 60 px at 720p: 2 each (58 px)
    CHECK(ModulePixels(29, 1.0f) == 2);
    // 1080p's 90 px: 3 each (87 px)
    CHECK(ModulePixels(29, 1.5f) == 3);
    // 4K's 180 px: 6 each
    CHECK(ModulePixels(29, 3.0f) == 6);
    // a tiny window still draws one
    CHECK(ModulePixels(29, 0.1f) == 1);
}

TEST_CASE("a line: glyphs where the font puts them, kerned, a third of the height for a missing one") {
    const auto font = TwoCellFont();
    // 40 px tall: twice the cell, so a cell is 20 px across
    const Line line = Layout(font, "AB", 40, 20, 20);
    REQUIRE(line.glyphs.size() == 2);
    CHECK(line.glyphs[0].x == 0);
    CHECK(line.glyphs[0].w == doctest::Approx(20));
    CHECK(line.glyphs[0].src_x == 0);
    CHECK(line.glyphs[0].src_w == doctest::Approx(10));
    CHECK(line.glyphs[0].src_h == doctest::Approx(20));
    // advanced a cell, then kerned back a tenth of one
    CHECK(line.glyphs[1].x == doctest::Approx(18));
    CHECK(line.glyphs[1].src_x == doctest::Approx(10));
    CHECK(line.width == doctest::Approx(38));

    const Line gap = Layout(font, "A?A", 30, 20, 20);
    REQUIRE(gap.glyphs.size() == 2);
    // a cell is 15 px; '?' moves on 10 px and isn't kerned against
    CHECK(gap.glyphs[1].x == doctest::Approx(25));
}

TEST_CASE("a line drawn: the texture's alpha averaged over each pixel") {
    const auto font = TwoCellFont();
    const auto texture = TwoCellTexture();
    // half the cell's size: each mask pixel covers 2x2 texels
    const Mask mask = Rasterize(Layout(font, "AB", 10, 20, 20), texture, 10);
    // A is 5 px, B starts at 4.5 (kerned) and ends at 9.5
    REQUIRE(mask.width == 10);
    REQUIRE(mask.height == 10);
    REQUIRE(mask.alpha.size() == 100);
    // inside A: opaque; past it, B's clear texels
    CHECK(mask.alpha[5 * 10 + 1] == 255);
    CHECK(mask.alpha[5 * 10 + 8] == 0);
    // where they overlap, A's stronger alpha wins
    CHECK(mask.alpha[5 * 10 + 4] == 255);
}

TEST_CASE("a line drawn: nothing for an empty line or no height") {
    const auto font = TwoCellFont();
    const auto texture = TwoCellTexture();
    CHECK(Rasterize(Layout(font, "", 10, 20, 20), texture, 10).width == 0);
    CHECK(Rasterize(Layout(font, "A", 10, 20, 20), texture, 0).alpha.empty());
}
