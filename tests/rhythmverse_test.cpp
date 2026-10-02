// Checks reading RhythmVerse's search replies and what band3 makes of them
// (src/Net/rhythmverse.cpp), with replies cut down from real ones.

#include <doctest/doctest.h>
#include <string>
#include "src/Game/song_id.h"
#include "src/Net/json.h"
#include "src/Net/rhythmverse.h"

using namespace band3::rhythmverse;

namespace {

// RhythmVerse's reply to a search, with fewer fields: a song it hosts, one on
// MediaFire, an official song (the Xbox store), a zipped upload, and an entry
// without a usable file ID
constexpr std::string_view kReply = R"json({"status":"success","data":{
  "records":{"total_available":51043,"total_filtered":812,"returned":5},
  "pagination":{"start":25,"records":"25","page":"2"},
  "songs":[
    {"data":{"artist":"Rob Zombie","title":"Never Gonna Stop","album":"The Sinister Urge",
             "genre":"Rock","year":2001,"song_length":190,"vocal_parts":"1",
             "diff_drums":"4","diff_guitar":"4","diff_bass":"3","diff_vocals":"1",
             "diff_keys":"0","diff_prokeys":"0","diff_proguitar":"-1","diff_probass":"-1",
             "diff_band":"4","album_art":"/assets/album_art/n/never-gonna-stop-16060.png"},
     "file":{"file_id":"595481a7cbc158.68319817","file_name":"DVNeverGonnaStopFinal",
             "file_title":"Never Gonna Stop (The Red, Red Kroovy)","file_artist":"Rob Zombie",
             "file_album":"The Sinister Urge","file_genre":"Rock","file_year":2001,
             "song_length":190,"vocal_parts_authored":"1","size":3706880,"downloads":2702,
             "zippata":0,"external_url":"","custom_id":"1689100131",
             "diff_drums":4,"diff_guitar":4,"diff_bass":3,"diff_vocals":1,"diff_proguitar":-1,
             "diff_probass":-1,"diff_band":4,
             "author":{"name":"DenVaktare"},"user":"denvaktare",
             "album_art":"/assets/album_art/denvaktare/595481a7cbc158.68319817.png",
             "file_url":"/songfile/595481a7cbc158.68319817",
             "download_url":"/download_file/denvaktare/595481a7cbc158.68319817/DVNeverGonnaStopFinal"}},
    {"data":{"artist":"Band","title":"Elsewhere","diff_guitar":"7"},
     "file":{"file_id":"5b947da4929df7.93833456","file_name":"x","zippata":0,
             "external_url":"https://www.mediafire.com/file/abc/x.rar","author":null,"user":"someone",
             "download_url":"https://www.mediafire.com/file/abc/x.rar","file_url":"/songfile/5b947da4929df7.93833456"}},
    {"data":{"artist":"DragonForce","title":"Through the Fire and Flames"},
     "file":{"file_id":"9b8fa4b5296d4091a6615c693c7b3953","external_url":null,
             "download_url":"http://marketplace.xbox.com/en-US/games/offers/0CCFF15A",
             "file_url":"/songfile/9b8fa4b5296d4091a6615c693c7b3953"}},
    {"data":{"artist":"Band","title":"Zipped"},
     "file":{"file_id":"60aa","zippata":1,"external_url":"",
             "download_url":"/download_file/u/60aa/pack.zip"}},
    {"data":{"artist":"Band","title":"Broken"},"file":{"file_id":"../../evil"}}
  ]}})json";

}

TEST_CASE("a search reply gives each song's details") {
    const auto result = ParseSearch(kReply);
    REQUIRE(result);
    CHECK(result->total == 812);
    CHECK(result->page == 2);
    REQUIRE(result->songs.size() == 4);  // the one without a usable file ID is left out

    const Song& s = result->songs[0];
    CHECK(s.file_id == "595481a7cbc158.68319817");
    // the upload's own fields over the song's
    CHECK(s.title == "Never Gonna Stop (The Red, Red Kroovy)");
    CHECK(s.artist == "Rob Zombie");
    CHECK(s.album == "The Sinister Urge");
    CHECK(s.genre == "Rock");
    CHECK(s.year == 2001);
    CHECK(s.length_s == 190);
    CHECK(s.vocal_parts == 1);
    CHECK(s.size == 3706880);
    CHECK(s.downloads == 2702);
    CHECK(s.author == "DenVaktare");
    CHECK(s.file_name == "DVNeverGonnaStopFinal");
    CHECK(s.song_id == 1689100131);
    CHECK(s.art_url == "https://rhythmverse.co/assets/album_art/denvaktare/595481a7cbc158.68319817.png");
    CHECK(s.page_url == "https://rhythmverse.co/songfile/595481a7cbc158.68319817");
    CHECK(s.download_url ==
          "https://rhythmverse.co/download_file/denvaktare/595481a7cbc158.68319817/DVNeverGonnaStopFinal");
    CHECK(s.host.empty());
}

TEST_CASE("difficulties become the game's tiers, and parts without one are left out") {
    const auto result = ParseSearch(kReply);
    REQUIRE(result);
    using Tiers = std::vector<std::pair<std::string, int32_t>>;
    // RhythmVerse's 1-7 are the game's 0-6; 0 and -1 are no part. Keys come
    // from the song's fields, since the upload has none.
    CHECK(result->songs[0].tiers ==
          Tiers{{"band", 3}, {"guitar", 3}, {"bass", 2}, {"drum", 3}, {"vocals", 0}});
    CHECK(result->songs[1].tiers == Tiers{{"guitar", 6}});
}

TEST_CASE("only songs RhythmVerse hosts unzipped can be downloaded") {
    const auto result = ParseSearch(kReply);
    REQUIRE(result);
    CHECK(result->songs[1].download_url.empty());
    CHECK(result->songs[1].host == "www.mediafire.com");
    CHECK(result->songs[1].author == "someone");  // no author record: the uploader's name
    CHECK(result->songs[2].download_url.empty());
    CHECK(result->songs[2].host == "marketplace.xbox.com");
    CHECK(result->songs[3].download_url.empty());
    CHECK(result->songs[3].host == "rhythmverse.co");
}

TEST_CASE("a zipped upload is one whatever type its flag comes as") {
    const auto result = ParseSearch(R"json({"status":"success","data":{"songs":[
        {"data":{},"file":{"file_id":"a1","zippata":"1","download_url":"/download_file/u/a1/x"}},
        {"data":{},"file":{"file_id":"a2","zippata":"0","download_url":"/download_file/u/a2/x"}},
        {"data":{},"file":{"file_id":"a3","zippata":true,"download_url":"/download_file/u/a3/x"}}]}})json");
    REQUIRE(result);
    REQUIRE(result->songs.size() == 3);
    CHECK(result->songs[0].download_url.empty());
    CHECK(!result->songs[1].download_url.empty());
    CHECK(result->songs[2].download_url.empty());
}

TEST_CASE("replies that aren't a successful search fail") {
    CHECK(!ParseSearch(""));
    CHECK(!ParseSearch("<html>Cloudflare</html>"));
    CHECK(!ParseSearch(R"({"status":"error","data":{"songs":[]}})"));
    CHECK(!ParseSearch(R"({"status":"success","data":{}})"));
    const auto empty = ParseSearch(R"({"status":"success","data":{"songs":[]}})");
    REQUIRE(empty);
    CHECK(empty->songs.empty());
    CHECK(empty->page == 1);
    // what a search that finds nothing gets
    const auto none = ParseSearch(
        R"({"status":"success","data":{"records":{"total_filtered":0},"songs":false}})");
    REQUIRE(none);
    CHECK(none->songs.empty());
    CHECK(none->total == 0);
}

TEST_CASE("custom_id is the song ID the game gives the song") {
    CHECK(SongIdOf("1689100131") == 1689100131);
    CHECK(SongIdOf("") == 0);
    CHECK(SongIdOf("0") == 0);
    CHECK(SongIdOf("-5") == 0);
    CHECK(SongIdOf("99999999999") == 0);  // more than a song ID holds
    // a text song_id, as band3 numbers it
    CHECK(SongIdOf("mysong") == band3::CorrectedSongId("mysong"));
    CHECK(SongIdOf("123abc") == band3::CorrectedSongId("123abc"));
}

TEST_CASE("a song is downloaded when its file is in the content folders, however it got there") {
    Song song;
    song.file_id = "595481a7cbc158.68319817";
    song.file_name = "DVNeverGonnaStopFinal";
    song.size = 3706880;
    LocalSongs local;
    CHECK(!IsDownloaded(song, local));

    // band3's own download, whatever its size
    local.files = {{"dvnevergonnastopfinal_595481a7cbc158.68319817", 99}};
    CHECK(IsDownloaded(song, local));

    // RhythmVerse's file name with the upload's size, in any case
    local.files = {{"dvnevergonnastopfinal", 3706880}};
    CHECK(IsDownloaded(song, local));
    // another size is another upload by that name, or another version
    local.files = {{"dvnevergonnastopfinal", 3706881}};
    CHECK(!IsDownloaded(song, local));
    // without a size to compare, a name isn't enough
    song.size = 0;
    local.files = {{"dvnevergonnastopfinal", 0}};
    CHECK(!IsDownloaded(song, local));
}

TEST_CASE("a search with text asks the search, and one without the newest songs") {
    SearchOptions options;
    options.text = "rob zombie & co";
    options.page = 3;
    const auto search = Search(options);
    CHECK(search.url == "https://rhythmverse.co/api/rb3xbox/songfiles/search/live");
    CHECK(search.form == "records=25&page=3&data_type=full&text=rob%20zombie%20%26%20co");
    CHECK(search.page_size == 25);

    const auto newest = Search(SearchOptions{});
    CHECK(newest.url == "https://rhythmverse.co/api/rb3xbox/songfiles/list");
    CHECK(newest.form ==
          "records=25&page=1&data_type=full&sort%5B0%5D%5Bsort_by%5D=release_date&"
          "sort%5B0%5D%5Bsort_order%5D=DESC");
}

TEST_CASE("a search's sort and filters become RhythmVerse's form fields") {
    SearchOptions options;
    options.text = "x";
    options.sort = "downloads";
    options.downloadable_only = true;
    options.has = {"real_keys", "keys"};
    options.harmonies = true;
    options.genres = {"metal"};
    options.decades = {1990};
    options.cap_part = "drum";
    options.cap_tier = 2;
    const auto search = Search(options);
    // band3 leaves out what it can't download, so it asks for more at once
    CHECK(search.page_size == 100);
    CHECK(search.form ==
          "records=100&page=1&data_type=full&text=x"
          "&sort%5B0%5D%5Bsort_by%5D=downloads&sort%5B0%5D%5Bsort_order%5D=DESC"
          "&instrument%5B%5D=prokeys&instrument%5B%5D=keys"
          "&vocal_parts%5B%5D=2&vocal_parts%5B%5D=3&genre%5B%5D=metal&decade%5B%5D=1990"
          // Solid (2) at most: RhythmVerse's tiers 1 to 3
          "&tierinstrument%5B%5D=drums&tier%5B%5D=1&tier%5B%5D=2&tier%5B%5D=3");
}

TEST_CASE("the page's query becomes a search's options, leaving out what isn't one") {
    const auto options = ParseSearchOptions(
        "/rv/search?text=never%20gonna&page=2&sort=title&downloadable=1"
        "&has=keys,real_guitar,banjo&harmonies=1&genre=metal,Bad-Genre,,rock"
        "&decade=1990,1995,abc,2000&cap=drum:4");
    CHECK(options.text == "never gonna");
    CHECK(options.page == 2);
    CHECK(options.sort == "title");
    CHECK(options.downloadable_only);
    CHECK(options.has == std::vector<std::string>{"keys", "real_guitar"});
    CHECK(options.harmonies);
    CHECK(options.genres == std::vector<std::string>{"metal", "rock"});
    CHECK(options.decades == std::vector<int32_t>{1990, 2000});
    CHECK(options.cap_part == "drum");
    CHECK(options.cap_tier == 4);

    const auto plain = ParseSearchOptions("/rv/search?sort=random&page=0&cap=drum:9&downloadable=yes");
    CHECK(plain.text.empty());
    CHECK(plain.page == 1);
    CHECK(plain.sort.empty());
    CHECK(!plain.downloadable_only);
    CHECK(plain.cap_tier == -1);
    CHECK(ParseSearchOptions("/rv/search?cap=banjo:2").cap_tier == -1);
}

TEST_CASE("form encoding keeps only unreserved characters") {
    CHECK(FormEncode("aZ09-._~") == "aZ09-._~");
    CHECK(FormEncode("a b+c=d&e") == "a%20b%2Bc%3Dd%26e");
    CHECK(FormEncode("\xC3\xA9") == "%C3%A9");
}

TEST_CASE("file IDs are letters, digits and dots") {
    CHECK(ValidFileId("595481a7cbc158.68319817"));
    CHECK(ValidFileId("9b8fa4b5296d4091a6615c693c7b3953"));
    CHECK(!ValidFileId(""));
    CHECK(!ValidFileId("../x"));
    CHECK(!ValidFileId("a/b"));
    CHECK(!ValidFileId("a_b"));
    CHECK(!ValidFileId(std::string(65, 'a')));
}

TEST_CASE("a download's file name is its uploaded name made safe, then its file ID") {
    Song song;
    song.file_id = "595481a7cbc158.68319817";
    song.file_name = "DVNeverGonnaStopFinal";
    CHECK(DownloadFileName(song) == "DVNeverGonnaStopFinal_595481a7cbc158.68319817");
    song.file_name = "a/b\\c:d*?\"<>|\xC3\xA9 (x).";
    CHECK(DownloadFileName(song) == "a_b_c_d________ (x)_595481a7cbc158.68319817");
    song.file_name = "...";
    CHECK(DownloadFileName(song) == "song_595481a7cbc158.68319817");
    song.file_name = std::string(100, 'n');
    CHECK(DownloadFileName(song) == std::string(60, 'n') + "_595481a7cbc158.68319817");

}

TEST_CASE("search results are JSON for the page, saying what's downloadable and downloaded") {
    const auto result = ParseSearch(kReply);
    REQUIRE(result);
    LocalSongs local;
    local.files = {{"dvnevergonnastopfinal", 3706880}};
    local.game_ids = std::set<int32_t>{1689100131};
    const auto page = band3::json::Parse(FormatSearch(*result, local));
    REQUIRE(page);
    CHECK((*page)["total"].Number() == 812);
    CHECK((*page)["page"].Number() == 2);
    CHECK((*page)["page_size"].Number() == kPageSize);
    const auto& songs = (*page)["songs"].Items();
    REQUIRE(songs.size() == 4);
    CHECK(songs[0]["title"].Text() == "Never Gonna Stop (The Red, Red Kroovy)");
    CHECK(songs[0]["length_ms"].Number() == 190000);
    CHECK(songs[0]["tiers"]["drum"].Number() == 3);
    CHECK(songs[0]["tiers"]["keys"].IsNull());
    CHECK(songs[0]["download"].Bool() == true);
    CHECK(songs[0]["downloaded"].Bool() == true);
    CHECK(songs[0]["in_library"].Bool() == true);
    CHECK(songs[0]["song_id"].Number() == 1689100131);
    CHECK(songs[1]["song_id"].Number() == 0);
    CHECK(songs[1]["in_library"].Bool(true) == false);
    CHECK(songs[0]["page"].Text() == "https://rhythmverse.co/songfile/595481a7cbc158.68319817");
    CHECK(songs[1]["download"].Bool(true) == false);
    CHECK(songs[1]["downloaded"].Bool(true) == false);
    CHECK(songs[1]["host"].Text() == "www.mediafire.com");

    // the game busy: nothing said of what it has
    local.game_ids.reset();
    const auto busy = band3::json::Parse(FormatSearch(*result, local));
    REQUIRE(busy);
    CHECK((*busy)["songs"].Items()[0]["in_library"].IsNull());
}

TEST_CASE("downloads are JSON, with their state and progress") {
    std::vector<Download> downloads(2);
    downloads[0].file_id = "a.1";
    downloads[0].title = "Song \"One\"";
    downloads[0].state = Download::State::kDownloading;
    downloads[0].received = 100;
    downloads[0].total = 400;
    downloads[1].file_id = "b.2";
    downloads[1].state = Download::State::kFailed;
    downloads[1].error = "the site answered 404";
    downloads[0].song_id = 7;
    const auto json = band3::json::Parse(
        FormatDownloads(downloads, "C:\\songs\\rhythmverse", std::set<int32_t>{7}));
    REQUIRE(json);
    CHECK((*json)["folder"].Text() == "C:\\songs\\rhythmverse");
    const auto& items = (*json)["downloads"].Items();
    REQUIRE(items.size() == 2);
    CHECK(items[0]["title"].Text() == "Song \"One\"");
    CHECK(items[0]["state"].Text() == "downloading");
    CHECK(items[0]["received"].Number() == 100);
    CHECK(items[0]["total"].Number() == 400);
    CHECK(items[1]["state"].Text() == "failed");
    CHECK(items[1]["error"].Text() == "the site answered 404");
    CHECK(items[0]["in_library"].Bool() == true);
    CHECK(items[0]["song_id"].Number() == 7);
    CHECK(items[1]["in_library"].Bool(true) == false);
    // the game busy, or nothing to ask it about
    const auto unknown = band3::json::Parse(FormatDownloads(downloads, "", std::nullopt));
    REQUIRE(unknown);
    CHECK((*unknown)["downloads"].Items()[0]["in_library"].IsNull());
}
