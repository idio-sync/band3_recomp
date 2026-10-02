// band3_write_config: writes band3.toml through the launcher's writer
// (src/Launcher/config_file.cpp), for the harness test that starts the game
// with it (tools/test_config_file.py).
//
//   band3_write_config <file> <name>=<value>...
//
// A value of true or false is written as a bool, one that reads as a whole
// number as an integer, one with a decimal point as a float, and anything else
// as a string, as the launcher writes each setting by its type. Like the
// launcher's Save, the file's other keys are kept. Windows hands the arguments
// over in its ANSI code page, so keep them to ASCII.

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>
#include "src/Launcher/config_file.h"

namespace {

using band3::launcher::ConfigValue;

template <typename T>
bool Parse(std::string_view text, T& out) {
    const auto [end, ec] = std::from_chars(text.data(), text.data() + text.size(), out);
    return ec == std::errc() && end == text.data() + text.size();
}

ConfigValue Typed(std::string_view text) {
    if (text == "true") return true;
    if (text == "false") return false;
    int64_t i = 0;
    if (Parse(text, i)) return i;
    double d = 0;
    if (text.find('.') != std::string_view::npos && Parse(text, d)) return d;
    return std::string(text);
}

}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: band3_write_config <file> <name>=<value>...\n");
        return 2;
    }
    std::vector<band3::launcher::ConfigEdit> edits;
    for (int i = 2; i < argc; i++) {
        const std::string_view arg = argv[i];
        const size_t equals = arg.find('=');
        if (equals == 0 || equals == std::string_view::npos) {
            std::fprintf(stderr, "not <name>=<value>: %s\n", argv[i]);
            return 2;
        }
        edits.push_back({std::string(arg.substr(0, equals)), Typed(arg.substr(equals + 1))});
    }
    const auto result = band3::launcher::SaveConfigFile(argv[1], edits);
    if (!result.ok) {
        std::fprintf(stderr, "%s\n", result.error.c_str());
        return 1;
    }
    return 0;
}
