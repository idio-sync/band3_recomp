// Checks reading a .mogg's header and its Ogg stream's packets
// (src/Video/mogg.cpp), and finding a song's audio in its package
// (src/Video/song_audio.cpp).

#include <doctest/doctest.h>
#include <cstdint>
#include <string>
#include <vector>
#include "src/Video/mogg.h"
#include "src/Video/song_audio.h"

using namespace band3::video;

namespace {

using Bytes = std::vector<uint8_t>;

void Le32(Bytes& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
}

// an Ogg page of `serial` with these lacing values over `body` (the CRC isn't
// checked, so it's left 0)
Bytes Page(uint32_t serial, uint8_t type, const Bytes& lacing, const Bytes& body) {
    Bytes out = {'O', 'g', 'g', 'S', 0, type};
    for (int i = 0; i < 8; i++) out.push_back(0);  // granule position
    Le32(out, serial);
    Le32(out, 0);  // sequence number
    Le32(out, 0);  // CRC
    out.push_back(uint8_t(lacing.size()));
    out.insert(out.end(), lacing.begin(), lacing.end());
    out.insert(out.end(), body.begin(), body.end());
    return out;
}

Bytes Fill(size_t n, uint8_t v) { return Bytes(n, v); }

Bytes Join(std::initializer_list<Bytes> parts) {
    Bytes out;
    for (const Bytes& p : parts) out.insert(out.end(), p.begin(), p.end());
    return out;
}

std::vector<Bytes> All(const Bytes& ogg) {
    OggPackets packets(ogg);
    std::vector<Bytes> out;
    Bytes p;
    while (packets.Next(p)) out.push_back(p);
    return out;
}

}

TEST_CASE("a mogg's header gives its version and where its Ogg starts") {
    Bytes mogg;
    Le32(mogg, 0x0A);
    Le32(mogg, 36);
    mogg.resize(40, 0);
    const auto h = ParseMoggHeader(mogg);
    REQUIRE(h);
    CHECK(h->version == kMoggUnencrypted);
    CHECK(h->ogg_offset == 36);
    // too short, or its Ogg past the end
    CHECK_FALSE(ParseMoggHeader(Bytes(7, 0)));
    Bytes past;
    Le32(past, 0x0A);
    Le32(past, 100);
    CHECK_FALSE(ParseMoggHeader(past));
}

TEST_CASE("an Ogg stream's packets are joined across pages, in order") {
    // a page with two packets and the start of a third, which ends on the next
    // page (lacing 255 runs on); a packet exactly 255 long ends with a 0
    const Bytes ogg = Join({
        Page(7, 0x02, {3, 2, 255}, Join({Fill(3, 1), Fill(2, 2), Fill(255, 3)})),
        Page(7, 0x01, {10, 255, 0}, Join({Fill(10, 3), Fill(255, 4)})),
    });
    const auto packets = All(ogg);
    REQUIRE(packets.size() == 4);
    CHECK(packets[0] == Fill(3, 1));
    CHECK(packets[1] == Fill(2, 2));
    CHECK(packets[2] == Join({Fill(255, 3), Fill(10, 3)}));
    CHECK(packets[3] == Fill(255, 4));
}

TEST_CASE("an Ogg stream's other streams are passed over, and a broken page ends it") {
    Bytes ogg = Join({
        Page(7, 0x02, {1}, {1}),
        Page(9, 0x02, {1}, {9}),
        Page(7, 0x00, {1}, {2}),
    });
    auto packets = All(ogg);
    REQUIRE(packets.size() == 2);
    CHECK(packets[1] == Bytes{2});
    // a page cut short: what came before stays
    ogg.resize(ogg.size() - 1);
    CHECK(All(ogg).size() == 1);
    // not a page at all
    CHECK(All(Bytes{'x', 'y', 'z'}).empty());
    // starting partway through a packet: that packet's end is passed over
    packets = All(Join({Page(7, 0x01, {5, 2}, Join({Fill(5, 8), Fill(2, 6)}))}));
    REQUIRE(packets.size() == 1);
    CHECK(packets[0] == Fill(2, 6));
}

TEST_CASE("Vorbis's headers become FFmpeg's extradata in Xiph lacing") {
    const Bytes id = Fill(30, 1), comment = Fill(300, 2), setup = Fill(5, 3);
    const Bytes x = XiphExtradata(id, comment, setup);
    REQUIRE(x.size() == 1 + 1 + 2 + 30 + 300 + 5);
    CHECK(x[0] == 2);
    CHECK(x[1] == 30);
    CHECK(x[2] == 255);
    CHECK(x[3] == 45);
    CHECK(x[4] == 1);
    CHECK(x[4 + 30] == 2);
    CHECK(x.back() == 3);
}

TEST_CASE("a song's audio is found in the first package with its shortname") {
    using band3::content::DtaSong;
    using band3::content::PackageSongs;
    PackageSongs a, b, c;
    a.path = "C:\\songs\\pack";
    a.songs = {DtaSong{"other", 1, "Other", "X"}};
    b.path = "C:\\songs\\mop";
    b.songs = {DtaSong{"MasterOfPuppets", 2, "Master of Puppets", "Metallica"}};
    b.songs[0].song_path = "songs/mop/mop";
    c.path = "C:\\songs\\copy";
    c.songs = b.songs;
    c.unreadable = false;
    const std::vector<PackageSongs> packages = {a, b, c};
    // the video's name may differ in case from the shortname
    const auto found = FindSongMogg(packages, "masterofpuppets");
    REQUIRE(found);
    CHECK(found->package == "C:\\songs\\mop");
    CHECK(found->mogg == "songs\\mop\\mop.mogg");
    // without a song path, the usual one
    const auto usual = FindSongMogg(packages, "other");
    REQUIRE(usual);
    CHECK(usual->mogg == "songs\\other\\other.mogg");
    CHECK_FALSE(FindSongMogg(packages, "nooneknows"));
    CHECK_FALSE(FindSongMogg(packages, ""));
}
