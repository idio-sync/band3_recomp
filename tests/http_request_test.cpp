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
