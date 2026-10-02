// Checks the web server's requests and replies (src/Net/http_request.cpp):
// RB3Enhanced's endpoints and formats, which its page and tools expect.

#include <doctest/doctest.h>
#include <string>
#include "src/Net/http_request.h"

using namespace band3::http;

TEST_CASE("a request line gives its method and target") {
    const auto request = ParseRequest("GET /list_songs HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(request);
    CHECK(request->method == "GET");
    CHECK(request->target == "/list_songs");

    CHECK(ParseRequest("POST /execute?script=x HTTP/1.0\r\n\r\n")->method == "POST");
    CHECK(!ParseRequest("GET /\r\n\r\n"));
    CHECK(!ParseRequest("GET  HTTP/1.1\r\n\r\n"));
    CHECK(!ParseRequest("GET / SPDY\r\n\r\n"));
    CHECK(!ParseRequest(""));
}

TEST_CASE("percent escapes decode and plus signs stay") {
    CHECK(UrlDecode("a%20b%2Fc") == "a b/c");
    CHECK(UrlDecode("%7b+ 1 2%7D") == "{+ 1 2}");
    // a broken escape is kept as it was sent
    CHECK(UrlDecode("100%") == "100%");
    CHECK(UrlDecode("%4") == "%4");
    CHECK(UrlDecode("%zz") == "%zz");
    CHECK(UrlDecode("%c3%a9") == "\xC3\xA9");
}

TEST_CASE("targets pick RB3E's endpoints") {
    CHECK(MatchRoute("/").endpoint == Endpoint::kIndex);
    CHECK(MatchRoute("/list_songs").endpoint == Endpoint::kListSongs);
    CHECK(MatchRoute("/jsonrpc").endpoint == Endpoint::kJsonRpc);
    CHECK(MatchRoute("/favicon.ico").endpoint == Endpoint::kNotFound);
    CHECK(MatchRoute("/list_songs/").endpoint == Endpoint::kNotFound);

    const Route song = MatchRoute("/song_1234");
    CHECK(song.endpoint == Endpoint::kSong);
    CHECK(song.song_id == 1234);
    CHECK(MatchRoute("/song_").endpoint == Endpoint::kNotFound);
    CHECK(MatchRoute("/song_12x").endpoint == Endpoint::kNotFound);

    const Route jump = MatchRoute("/jump?shortname=gimmeshelter%26co");
    CHECK(jump.endpoint == Endpoint::kJump);
    CHECK(jump.argument == "gimmeshelter&co");

    const Route script = MatchRoute("/execute?script=%7Bprint%20hi%7D");
    CHECK(script.endpoint == Endpoint::kExecute);
    CHECK(script.argument == "{print hi}");

    const Route art = MatchRoute("/album_art?shortname=gimmeshelter%26co");
    CHECK(art.endpoint == Endpoint::kAlbumArt);
    CHECK(art.argument == "gimmeshelter&co");
    CHECK(MatchRoute("/album_art").endpoint == Endpoint::kNotFound);

    CHECK(MatchRoute("/status").endpoint == Endpoint::kStatus);
    CHECK(MatchRoute("/song_details").endpoint == Endpoint::kSongDetails);
}

TEST_CASE("a request's Content-Type and Content-Length are read, whatever their case") {
    const auto post = ParseRequest(
        "POST /rv/download HTTP/1.1\r\nHost: x\r\ncontent-TYPE:  application/json \r\n"
        "Content-Length: 27\r\n\r\n");
    REQUIRE(post);
    CHECK(post->content_type == "application/json");
    CHECK(post->content_length == 27);
    CHECK(post->body.empty());

    const auto get = ParseRequest("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
    REQUIRE(get);
    CHECK(get->content_type.empty());
    CHECK(get->content_length == 0);

    CHECK(!ParseRequest("POST / HTTP/1.1\r\nContent-Length: lots\r\n\r\n"));
    CHECK(!ParseRequest("POST / HTTP/1.1\r\nContent-Length: -1\r\n\r\n"));
    CHECK(!ParseRequest("POST / HTTP/1.1\r\nContent-Length:\r\n\r\n"));
}

TEST_CASE("query parameters are found by name and decoded, '+' as a space") {
    CHECK(QueryParam("/x?text=rob+zombie%26co&page=2", "text") == "rob zombie&co");
    CHECK(QueryParam("/x?text=rob+zombie%26co&page=2", "page") == "2");
    CHECK(QueryParam("/x?text=a&page=2", "nope") == std::nullopt);
    CHECK(QueryParam("/x", "text") == std::nullopt);
    CHECK(QueryParam("/x?flag&text=", "flag") == "");
    CHECK(QueryParam("/x?flag&text=", "text") == "");
    // a name that only starts the same isn't it
    CHECK(QueryParam("/x?texts=a", "text") == std::nullopt);
}

TEST_CASE("targets pick the RhythmVerse endpoints") {
    const Route search = MatchRoute("/rv/search?text=never%20gonna&page=3");
    CHECK(search.endpoint == Endpoint::kRvSearch);
    CHECK(search.argument == "never gonna");
    CHECK(search.page == 3);

    const Route newest = MatchRoute("/rv/search");
    CHECK(newest.endpoint == Endpoint::kRvSearch);
    CHECK(newest.argument.empty());
    CHECK(newest.page == 1);
    CHECK(MatchRoute("/rv/search?page=0").page == 1);
    CHECK(MatchRoute("/rv/search?page=x").page == 1);

    CHECK(MatchRoute("/rv/download").endpoint == Endpoint::kRvDownload);
    CHECK(MatchRoute("/rv/downloads").endpoint == Endpoint::kRvDownloads);
    CHECK(MatchRoute("/rv/searches").endpoint == Endpoint::kNotFound);
    CHECK(MatchRoute("/rv/").endpoint == Endpoint::kNotFound);
}

TEST_CASE("song details are JSON keyed by shortname, with the parts each song has") {
    SongDetails rehab{"rehab", "R&B/Soul/Funk", 2006, 214000, 1,
                      {{"band", 3}, {"guitar", 2}, {"vocals", 4}}};
    SongDetails mot{"mot", "Mot\xF6rhead \"Metal\"", 0, 0, 0, {}};
    CHECK(FormatSongDetails({rehab, mot}) ==
          R"({"rehab":{"genre":"R&B/Soul/Funk","year":2006,"length_ms":214000,"vocal_parts":1,)"
          R"("tiers":{"band":3,"guitar":2,"vocals":4}},)"
          "\"mot\":{\"genre\":\"Mot\xC3\xB6rhead \\\"Metal\\\"\",\"year\":0,\"length_ms\":0,"
          R"("vocal_parts":0,"tiers":{}}})");
    CHECK(FormatSongDetails({}) == "{}");
}

TEST_CASE("the status is JSON, with what's playing or null") {
    Status idle;
    idle.screen = "main_hub_screen";
    CHECK(FormatStatus(idle) ==
          R"({"screen":"main_hub_screen","in_library":false,"playing":null})");

    Status playing;
    playing.screen = "song_select_screen";
    playing.in_library = true;
    playing.playing = Status::Playing{"rehab", "Rehab", "Amy Winehouse", 123450, 61000, 214000};
    CHECK(FormatStatus(playing) ==
          R"({"screen":"song_select_screen","in_library":true,"playing":{"shortname":"rehab",)"
          R"("title":"Rehab","artist":"Amy Winehouse","score":123450,"position_ms":61000,)"
          R"("length_ms":214000}})");
}

TEST_CASE("JSON strings are escaped, and Latin-1 becomes UTF-8") {
    Status status;
    status.playing = Status::Playing{"x", "Say \"Hi\"\\\n", "Mot\xF6rhead\x01", 0, -1, 0};
    const std::string json = FormatStatus(status);
    CHECK(json.find(R"("title":"Say \"Hi\"\\\n")") != std::string::npos);
    CHECK(json.find("\"artist\":\"Mot\xC3\xB6rhead\\u0001\"") != std::string::npos);
    // an unknown position is null
    CHECK(json.find(R"("position_ms":null)") != std::string::npos);
}

TEST_CASE("songs are written as RB3E writes them") {
    const SongInfo song{"cherubrock", "Cherub Rock", "Smashing Pumpkins", "Siamese Dream",
                        "rb2"};
    CHECK(FormatSong(song, false) ==
          "shortname=cherubrock\r\ntitle=Cherub Rock\r\nartist=Smashing Pumpkins\r\n"
          "album=Siamese Dream\r\norigin=rb2\r\n\r\n");
    CHECK(FormatSong(song, true).starts_with("[cherubrock]\r\nshortname=cherubrock\r\n"));

    // a line break in a value would start another key
    const SongInfo broken{"x", "Two\r\nLines", "", "", ""};
    CHECK(FormatSong(broken, false).find("title=Two  Lines\r\n") != std::string::npos);
}

TEST_CASE("Latin-1 strings become UTF-8 and UTF-8 stays as it is") {
    CHECK(ToUtf8("Motorhead") == "Motorhead");
    CHECK(ToUtf8("Mot\xF6rhead") == "Mot\xC3\xB6rhead");
    CHECK(ToUtf8("Mot\xC3\xB6rhead") == "Mot\xC3\xB6rhead");
    CHECK(ToUtf8("\xE2\x80\x99") == "\xE2\x80\x99");
    // a lone continuation byte or a cut-off sequence isn't UTF-8
    CHECK(ToUtf8("\xC3") == "\xC3\x83");
    CHECK(ToUtf8("a\x80") == "a\xC2\x80");
}

TEST_CASE("replies carry their length, type and CORS when asked") {
    const std::string ok = Response(200, "text/plain", "OK", false);
    CHECK(ok.starts_with("HTTP/1.1 200 OK\r\n"));
    CHECK(ok.find("Content-Type: text/plain\r\n") != std::string::npos);
    CHECK(ok.find("Content-Length: 2\r\n") != std::string::npos);
    CHECK(ok.find("Access-Control-Allow-Origin") == std::string::npos);
    CHECK(ok.ends_with("\r\n\r\nOK"));

    const std::string cors = Response(409, "text/plain", "", true);
    CHECK(cors.starts_with("HTTP/1.1 409 Conflict\r\n"));
    CHECK(cors.find("Access-Control-Allow-Origin: *\r\n") != std::string::npos);
    CHECK(cors.find("Content-Length: 0\r\n") != std::string::npos);
}

TEST_CASE("replies aren't cached unless they say for how long") {
    CHECK(Response(200, "text/plain", "OK", false).find("Cache-Control: no-store\r\n") !=
          std::string::npos);
    const std::string art = Response(200, "image/jpeg", "x", false, 3600);
    CHECK(art.find("Cache-Control: max-age=3600\r\n") != std::string::npos);
    CHECK(art.find("no-store") == std::string::npos);
}
