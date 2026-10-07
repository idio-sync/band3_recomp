// Checks reading songs.dta (src/Content/song_dta.cpp) and finding duplicate
// songs among packages (src/Content/song_inventory.cpp).

#include <doctest/doctest.h>
#include <string>
#include "src/Content/song_dta.h"
#include "src/Content/song_inventory.h"
#include "src/Game/song_id.h"
#include "src/Net/json.h"

using namespace band3::content;

namespace {

// a custom song's songs.dta as Magma writes it, cut down
constexpr std::string_view kMagma = R"dta((
   'GTAVITheme'
   (
      'name'
      "Welcome To Vice City"
   )
   (
      'artist'
      "Rockstar Games"
   )
   ('master' 1)
   (
      'song'
      (
         'name'
         "songs/gtavitheme/gtavitheme"
      )
      ('tracks_count' (2 2 2 0 2 2))
      (mute_volume -96)
   )
   (drum_bank sfx/kit01_bank.milo)
   ('preview' 15000 48000)
   ('rank' ('drum' 1) ('guitar' 221))
   ('song_id' 2133017793)
   (solo (guitar))
   ('encoding' 'latin1')
   (author "Dannydsi3d")

;DO NOT EDIT THE FOLLOWING LINES MANUALLY
;Song=Welcome To Vice City
)
)dta";

}

TEST_CASE("a songs.dta gives each song's shortname, song ID, name and artist") {
    const auto songs = ParseSongsDta(kMagma);
    REQUIRE(songs.size() == 1);
    CHECK(songs[0].shortname == "GTAVITheme");
    CHECK(songs[0].song_id == 2133017793);
    CHECK(songs[0].title == "Welcome To Vice City");
    CHECK(songs[0].artist == "Rockstar Games");
}

TEST_CASE("a pack's songs.dta gives every song, in its order") {
    const auto songs = ParseSongsDta(R"dta(
; a pack, the official DLC's way: bare symbols, [] and {} as well as ()
#ifndef kTest
#define kTest (1)
#endif
(first (name "One") (artist "Band A") (song (name "songs/first/first")) (song_id 1001))
/* (commented (name "Out") (song (name "x")) (song_id 9)) */
[second (name "Two") (artist "Band B") {song (name "songs/second/second")} (song_id "2002")]
(third (name "Three") (artist "Band C") (song (name "songs/third/third")) (song_id text_id))
(not_a_song (name "Settings") (song_id 5))
)dta");
    REQUIRE(songs.size() == 3);
    CHECK(songs[0].shortname == "first");
    CHECK(songs[0].song_id == 1001);
    CHECK(songs[1].shortname == "second");
    CHECK(songs[1].song_id == 2002);
    CHECK(songs[1].artist == "Band B");
    // a text song_id, as the game numbers it
    CHECK(songs[2].song_id == band3::CorrectedSongId("text_id"));
}

TEST_CASE("a songs.dta's text becomes UTF-8 as the game reads it") {
    // Latin-1 unless the song says UTF-8
    auto songs = ParseSongsDta("(a (name \"Caf\xE9\") (artist \"x\") (song (name \"s\")))");
    REQUIRE(songs.size() == 1);
    CHECK(songs[0].title == "Caf\xC3\xA9");
    songs = ParseSongsDta("(a (name \"Caf\xC3\xA9\") (encoding utf8) (song (name \"s\")))");
    REQUIRE(songs.size() == 1);
    CHECK(songs[0].title == "Caf\xC3\xA9");
    // says UTF-8 but isn't
    songs = ParseSongsDta("(a (name \"Caf\xE9\") (encoding utf8) (song (name \"s\")))");
    REQUIRE(songs.size() == 1);
    CHECK(songs[0].title == "Caf\xC3\xA9");
}

TEST_CASE("what isn't a songs.dta gives no songs, or those before where it goes wrong") {
    CHECK(ParseSongsDta("").empty());
    CHECK(ParseSongsDta("\x89PNG\r\n").empty());
    CHECK(ParseSongsDta("(a (name \"never closed").empty());
    const auto some = ParseSongsDta(
        "(a (song (name \"s\")) (song_id 1)) ) (b (song (name \"s\")) (song_id 2)) (c (name \"open");
    REQUIRE(some.size() == 2);  // a stray ) is passed over
    CHECK(some[1].shortname == "b");
    std::string deep(200, '(');
    CHECK(ParseSongsDta(deep + "x" + std::string(200, ')')).empty());
    CHECK(ParseSongsDta("(a (song_id 99999999999) (song (name \"s\")))").at(0).song_id == 0);
}

namespace {

DtaSong Dta(std::string shortname, int32_t id, std::string title, std::string artist = "Artist") {
    return {std::move(shortname), id, std::move(title), std::move(artist)};
}

PackageSongs SongsIn(std::string path, std::vector<DtaSong> songs) {
    PackageSongs package;
    package.path = std::move(path);
    package.size = 1000;
    package.songs = std::move(songs);
    return package;
}

}

TEST_CASE("the cache reads back as written, times and all") {
    std::vector<PackageSongs> packages = {
        SongsIn("C:\\songs\\a \"quoted\"", {Dta("one", 1, "One"), Dta("two", 2, "Caf\xC3\xA9")}),
        SongsIn("C:\\songs\\b", {})};
    packages[0].modified = 133423456789012345;  // more than a double keeps exactly
    packages[1].unreadable = true;
    const auto back = ParseInventoryCache(FormatInventoryCache(packages));
    REQUIRE(back.size() == 2);
    CHECK(back[0].path == packages[0].path);
    CHECK(back[0].size == 1000);
    CHECK(back[0].modified == 133423456789012345);
    REQUIRE(back[0].songs.size() == 2);
    CHECK(back[0].songs[1].title == "Caf\xC3\xA9");
    CHECK(back[0].songs[1].song_id == 2);
    CHECK(back[1].unreadable);
    CHECK(ParseInventoryCache("").empty());
    CHECK(ParseInventoryCache(R"({"version": 2, "packages": [{"path": "x"}]})").empty());
}

TEST_CASE("songs with the same song_id: the game has the first loaded") {
    // as tried in the game: GTAVIThemX, with GTAVITheme's song_id, after it
    const std::vector<PackageSongs> packages = {
        SongsIn("a_gtavi", {Dta("GTAVITheme", 2133017793, "Welcome To Vice City")}),
        SongsIn("b_gtavi_sameid", {Dta("GTAVIThemX", 2133017793, "Welcome To Vice CitX")})};
    const auto groups = FindDuplicates(packages, {});
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].kind == DuplicateKind::kSongId);
    CHECK(groups[0].key == "2133017793");
    REQUIRE(groups[0].copies.size() == 2);
    CHECK(groups[0].copies[0].package == 0);
    CHECK(groups[0].copies[0].in_use);
    CHECK(groups[0].copies[1].package == 1);
    CHECK(!groups[0].copies[1].in_use);
}

TEST_CASE("songs with the same shortname clash, the disc's too") {
    const std::vector<PackageSongs> packages = {
        SongsIn("a_cuthands", {Dta("cut_hands", 2131221630, "cut hands", "Deftones")}),
        SongsIn("b_cuthands", {Dta("cut_hands", 2131221631, "cut handX", "Deftones")}),
        SongsIn("b_crazytrain", {Dta("crazytrain", 2133017799, "Welcome To Vice Ci3y")})};
    // the game lists the packages' songs and its own; only its own are new here
    const std::vector<GameSong> game = {{"crazytrain", 1012, "Crazy Train", "Ozzy Osbourne"},
                                        {"cut_hands", 2131221630, "cut hands", "Deftones"},
                                        {"cut_hands", 2131221631, "cut handX", "Deftones"}};
    const auto groups = FindDuplicates(packages, game);
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].kind == DuplicateKind::kShortname);
    CHECK(groups[0].key == "cut_hands");
    CHECK(groups[0].copies.size() == 2);
    CHECK(groups[0].copies[0].in_use);
    CHECK(groups[0].copies[1].in_use);  // both load
    CHECK(groups[1].key == "crazytrain");
    REQUIRE(groups[1].copies.size() == 2);
    CHECK(groups[1].copies[0].package == 2);
    CHECK(groups[1].copies[1].package == -1);  // the disc's
    CHECK(groups[1].copies[1].song.title == "Crazy Train");
    // without the game's songs, the disc's clash goes unseen
    CHECK(FindDuplicates(packages, {}).size() == 1);
}

TEST_CASE("another chart of the same artist and title is similar, and a pack counts") {
    const std::vector<PackageSongs> packages = {
        SongsIn("pack", {Dta("thebest_pack", 5001, "The Best!", "Some Band"), Dta("other", 5002, "Other")}),
        SongsIn("single", {Dta("thebest", 7001, "the best", "SOME BAND")})};
    const auto groups = FindDuplicates(packages, {});
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].kind == DuplicateKind::kSimilar);
    CHECK(groups[0].key == "Some Band - The Best!");
    CHECK(groups[0].copies.size() == 2);

    const auto json = band3::json::Parse(FormatDuplicates(groups, packages, {true, 1, 2, false}));
    REQUIRE(json);
    CHECK((*json)["reading"].Bool() == true);
    CHECK((*json)["read"].Number() == 1);
    CHECK((*json)["total"].Number() == 2);
    CHECK((*json)["game"].Bool(true) == false);
    const auto& copies = (*json)["groups"].Items()[0]["copies"].Items();
    REQUIRE(copies.size() == 2);
    CHECK((*json)["groups"].Items()[0]["kind"].Text() == "similar");
    CHECK(copies[0]["file"].Text() == "pack");
    CHECK(copies[0]["songs_in_file"].Number() == 2);
    CHECK(copies[0]["size"].Number() == 1000);
    CHECK(copies[0]["in_use"].Bool() == true);
    CHECK(copies[1]["shortname"].Text() == "thebest");
    CHECK((*json)["set_aside"].Items().empty());

    const auto listed = band3::json::Parse(
        FormatDuplicates({}, {}, {}, {{"C:\\songs\\Old_1", false}, {"C:\\songs\\Next_2", true}}));
    REQUIRE(listed);
    const auto& set_aside = (*listed)["set_aside"].Items();
    REQUIRE(set_aside.size() == 2);
    CHECK(set_aside[0]["file"].Text() == "C:\\songs\\Old_1");
    CHECK(set_aside[0]["next_launch"].Bool(true) == false);
    CHECK(set_aside[1]["next_launch"].Bool() == true);
}

TEST_CASE("copies of one package that band3 left out are the same file") {
    const std::vector<PackageSongs> packages = {
        SongsIn("songs\\Pack_rb3con", {Dta("one", 1, "One"), Dta("two", 2, "Two")}),
        SongsIn("songs\\Single_rb3con", {Dta("three", 3, "Three")})};
    const std::vector<LeftOutCopy> left_out = {{"backup\\Pack_rb3con", 1000, "songs\\Pack_rb3con"},
                                               {"old/Pack_rb3con", 999, "songs\\Pack_rb3con"},
                                               {"x", 5, "not one of them"}};
    const auto groups = FindDuplicates(packages, {}, left_out);
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].kind == DuplicateKind::kSameFile);
    CHECK(groups[0].key == "Pack_rb3con");
    REQUIRE(groups[0].copies.size() == 3);
    const auto& kept = groups[0].copies[0];
    CHECK(kept.in_use);
    CHECK(kept.package == 0);
    CHECK(kept.file == "songs\\Pack_rb3con");
    CHECK(kept.songs_in_file == 2);
    CHECK(kept.song.shortname == "one");  // the package's first song
    const auto& copy = groups[0].copies[1];
    CHECK(!copy.in_use);
    CHECK(copy.package == -1);
    CHECK(copy.file == "backup\\Pack_rb3con");
    CHECK(copy.songs_in_file == 2);
    CHECK(!copy.differs);
    CHECK(groups[0].copies[2].differs);  // another size: not quite the same

    const auto json = band3::json::Parse(FormatDuplicates(groups, packages, {}));
    REQUIRE(json);
    const auto& group = (*json)["groups"].Items()[0];
    CHECK(group["kind"].Text() == "same_file");
    CHECK(group["copies"].Items()[1]["file"].Text() == "backup\\Pack_rb3con");
    CHECK(group["copies"].Items()[1]["size"].Number() == 1000);
    CHECK(group["copies"].Items()[1]["songs_in_file"].Number() == 2);
    CHECK(group["copies"].Items()[2]["differs"].Bool() == true);
    CHECK(group["copies"].Items()[0]["differs"].Bool(true) == false);
}

TEST_CASE("a song only once, or a clash said already, isn't listed again") {
    const std::vector<PackageSongs> packages = {
        SongsIn("a", {Dta("one", 1, "Same", "Band")}), SongsIn("b", {Dta("one", 2, "Same", "Band")}),
        SongsIn("c", {Dta("three", 3, "Three")})};
    const auto groups = FindDuplicates(packages, {});
    // a shortname clash, not also "similar"
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].kind == DuplicateKind::kShortname);

    CHECK(FindDuplicates({SongsIn("x", {Dta("solo", 9, "Solo")})}, {{"solo", 9, "Solo", "Artist"}}).empty());
}
