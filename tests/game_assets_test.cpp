// Checks the web server's /game_asset/milo_texture replies (src/Net/game_assets.cpp):
// what's read, decoded and kept, with a made-up milo and a stand-in for the game's
// file reads, so no game data is in the repository.

#include <doctest/doctest.h>
#include <cstdint>
#include <string>
#include <utility>
#include "src/Net/game_assets.h"
#include "src/Net/http_request.h"
#include "src/Net/milo.h"

using namespace band3::http;

namespace {

std::string Le32(uint32_t v) {
    std::string out;
    for (int i = 0; i < 4; i++) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}
std::string Be32(uint32_t v) {
    std::string out;
    for (int i = 3; i >= 0; i--) out += static_cast<char>((v >> (8 * i)) & 0xFF);
    return out;
}
std::string Be16(uint16_t v) { return {static_cast<char>(v >> 8), static_cast<char>(v & 0xFF)}; }
std::string Name(const std::string& s) { return Be32(static_cast<uint32_t>(s.size())) + s; }

// a 4x4 DXT5 bitmap as a milo embeds it, red and opaque
std::string Bitmap() {
    std::string block = {'\xFF', '\0', '\0', '\0', '\0', '\0', '\0', '\0',
                         '\0', '\xF8', '\0', '\0', '\0', '\0', '\0', '\0'};
    for (size_t i = 0; i < block.size(); i += 2) std::swap(block[i], block[i + 1]);
    return std::string("\x01\x08", 2) + Be32(24) + std::string(1, '\0') + Be16(4) + Be16(4) + Be16(4) +
           std::string(19, '\0') + block;
}

// a milo of one stored block: its objects, with one texture, a.png
std::string MiloFile() {
    const std::string objects = std::string("\x00\x00\x00\x1c", 4) + Name("a.png") + std::string(1, '\0') + Bitmap();
    std::string out = Le32(0xCDBEDEAF) + Le32(0x810) + Le32(1) + Le32(0x10000) +
                      Le32(static_cast<uint32_t>(objects.size()) | 0x01000000);
    out.resize(0x810, '\0');
    return out + objects;
}

Route Ask(const std::string& milo, const std::string& name) {
    Route route;
    route.endpoint = Endpoint::kGameMiloTexture;
    route.argument = milo;
    route.name = name;
    return route;
}

const std::string kMilo = "ui/track/gen/track_shared.milo_xbox";

// the game's file reads, counted: `busy` for a game that can't take them yet,
// `file` what each read gives
struct Reads {
    int count = 0;
    bool busy = false;
    std::optional<std::string> file = MiloFile();
    GameFileReader Reader() {
        return [this](const std::string&) {
            count++;
            return GameRead{busy, busy ? std::nullopt : file};
        };
    }
};

}

TEST_CASE("a milo's texture is read once, decoded to a PNG, and kept") {
    AssetCache assets(8);
    band3::milo::MiloCache milos(2);
    Reads reads;
    const auto a = MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader());
    REQUIRE(a);
    CHECK(a->status == 200);
    CHECK(a->type == "image/png");
    CHECK(a->body.starts_with("\x89PNG"));
    CHECK(MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader())->body == a->body);
    CHECK(reads.count == 1);
}

TEST_CASE("another texture of a milo already read doesn't read it again") {
    AssetCache assets(8);
    band3::milo::MiloCache milos(2);
    Reads reads;
    MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader());
    // not in it: a 404, kept, and from the milo already decompressed
    const auto b = MiloTexture(Ask(kMilo, "b.png"), assets, milos, reads.Reader());
    REQUIRE(b);
    CHECK(b->status == 404);
    CHECK(MiloTexture(Ask(kMilo, "b.png"), assets, milos, reads.Reader())->status == 404);
    CHECK(reads.count == 1);
}

TEST_CASE("milos and names outside the allowlists are 404s, and read nothing") {
    AssetCache assets(8);
    band3::milo::MiloCache milos(2);
    Reads reads;
    CHECK(MiloTexture(Ask("songs/x/gen/x.milo_xbox", "a.png"), assets, milos, reads.Reader())->status == 404);
    CHECK(MiloTexture(Ask("ui/../x.milo_xbox", "a.png"), assets, milos, reads.Reader())->status == 404);
    CHECK(MiloTexture(Ask(kMilo, "A.png"), assets, milos, reads.Reader())->status == 404);
    CHECK(MiloTexture(Ask(kMilo, ""), assets, milos, reads.Reader())->status == 404);
    CHECK(reads.count == 0);
}

TEST_CASE("a busy game gives no reply, and keeps nothing for later") {
    AssetCache assets(8);
    band3::milo::MiloCache milos(2);
    Reads reads;
    reads.busy = true;
    CHECK(!MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader()));
    reads.busy = false;
    const auto a = MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader());
    REQUIRE(a);
    CHECK(a->status == 200);
    CHECK(reads.count == 2);
}

TEST_CASE("a milo that isn't there, or isn't one, is a 404") {
    AssetCache assets(8);
    band3::milo::MiloCache milos(2);
    Reads reads;
    reads.file = std::nullopt;
    CHECK(MiloTexture(Ask(kMilo, "a.png"), assets, milos, reads.Reader())->status == 404);
    CHECK(reads.count == 1);
    Reads junk;
    junk.file = std::string(64, 'x');
    CHECK(MiloTexture(Ask("ui/other.milo_xbox", "a.png"), assets, milos, junk.Reader())->status == 404);
    CHECK(junk.count == 1);
}

TEST_CASE("the asset cache keeps the newest replies") {
    AssetCache assets(2);
    assets.Put("a", Asset{200, "image/png", "A"});
    assets.Put("b", Asset{200, "image/png", "B"});
    assets.Put("c", Asset{200, "image/png", "C"});
    CHECK(!assets.Get("a"));
    CHECK(assets.Get("b")->body == "B");
    CHECK(assets.Get("c")->body == "C");
    CHECK(Asset{}.status == 404);
}
