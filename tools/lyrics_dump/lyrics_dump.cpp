// band3_lyrics: what band3's /lyrics?shortname= serves for a song's MIDI file
// (src/Net/lyrics.h), to check a song against what the game shows, and for
// tools/web_preview.py's /karaoke. Built with the unit tests (tests/CMakeLists.txt).
//   band3_lyrics <song.mid>          the JSON, the file's name as the shortname
//   band3_lyrics <song.mid> --text   each part's lines, with their start times

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include "src/Net/lyrics.h"

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: band3_lyrics <song.mid> [--text]\n");
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        std::fprintf(stderr, "can't open %s\n", argv[1]);
        return 1;
    }
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const auto midi = band3::lyrics::ReadMidi(bytes);
    if (!midi) {
        std::fprintf(stderr, "%s isn't a MIDI file band3 can read\n", argv[1]);
        return 1;
    }
    const auto parts = band3::lyrics::FromMidi(*midi);
    if (argc < 3 || std::string_view(argv[2]) != "--text") {
        const std::string shortname = std::filesystem::path(argv[1]).stem().string();
        std::fputs(band3::lyrics::FormatJson(shortname, parts).c_str(), stdout);
        return 0;
    }
    for (const auto& part : parts) {
        std::printf("%s\n", part.part.c_str());
        for (const auto& line : part.lines) {
            std::printf("  %8.3f  ", line.start_ms / 1000.0);
            for (const auto& s : line.syllables) {
                std::fputs((s.text + (s.join ? "" : " ")).c_str(), stdout);
            }
            std::printf("\n");
        }
    }
    return 0;
}
