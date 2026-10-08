// band3_milo_font: what band3's /game_asset/font serves for a font's
// .milo_xbox (src/Net/milo.h), to check a font, and for tools/web_preview.py.
// Built with the unit tests (tests/CMakeLists.txt).
//   band3_milo_font <font.milo_xbox>              the JSON
//   band3_milo_font <font.milo_xbox> <out.png>    the JSON, and its texture as a PNG

#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include "src/Net/album_art.h"
#include "src/Net/milo.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: band3_milo_font <font.milo_xbox> [out.png]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }
    const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto raw = band3::milo::Decompress(file);
    if (!raw) {
        std::fprintf(stderr, "%s isn't a milo band3 can decompress\n", argv[1]);
        return 1;
    }
    const auto font = band3::milo::FindFont(*raw);
    const auto texture = band3::milo::FindBitmap(*raw);
    if (!font || !texture) {
        std::fprintf(stderr, "%s has no %s\n", argv[1], font ? "DXT texture" : "font band3 can read");
        return 1;
    }
    std::fputs(band3::milo::FormatFontJson(*font).c_str(), stdout);
    if (argc > 2) {
        std::ofstream out(argv[2], std::ios::binary);
        const std::string png = band3::http::EncodePng(*texture);
        out.write(png.data(), static_cast<std::streamsize>(png.size()));
    }
    return 0;
}
