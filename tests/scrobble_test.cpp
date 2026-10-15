// Checks what band3 tells Last.fm and ListenBrainz (src/Net/scrobble_protocol.cpp):
// MD5 against RFC 1321's vectors, when a song counts as listened to, Last.fm's
// signature and form, ListenBrainz's JSON, and what each service's replies mean.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Net/json.h"
#include "src/Net/md5.h"
#include "src/Net/scrobble_protocol.h"

using namespace band3::scrobble;
using band3::net::Md5Hex;

namespace {

Track Song(std::string title, std::string artist, int32_t length_ms, int64_t started) {
    return Track{std::move(title), std::move(artist), length_ms, started};
}

}

TEST_CASE("MD5 gives RFC 1321's digests") {
    CHECK(Md5Hex("") == "d41d8cd98f00b204e9800998ecf8427e");
    CHECK(Md5Hex("a") == "0cc175b9c0f1b6a831c399e269772661");
    CHECK(Md5Hex("abc") == "900150983cd24fb0d6963f7d28e17f72");
    CHECK(Md5Hex("message digest") == "f96b697d7cb7938d525a2f31aaf161d0");
    CHECK(Md5Hex("abcdefghijklmnopqrstuvwxyz") == "c3fcd3d76192e4007dfb496cca67e13b");
    CHECK(Md5Hex("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789") ==
          "d174ab98d277d9f5a5611c2c9f419d9f");
    // two blocks of padding: 80 bytes
    CHECK(Md5Hex("1234567890123456789012345678901234567890"
                 "1234567890123456789012345678901234567890") ==
          "57edf4a22be3c955ac49da2e2107b67a");
    // 56 bytes, where the length no longer fits in the first padding block
    CHECK(Md5Hex(std::string(56, 'a')) == "3b0c8ac703f828b04c6c197006d17218");
}

TEST_CASE("a song counts when played half its length or 4 minutes, if longer than 30 s") {
    CHECK_FALSE(ShouldScrobble(30000, 30));
    CHECK_FALSE(ShouldScrobble(0, 600));
    CHECK(ShouldScrobble(31000, 16));
    CHECK_FALSE(ShouldScrobble(31000, 15));
    CHECK(ShouldScrobble(200000, 100));
    CHECK_FALSE(ShouldScrobble(200000, 99));
    // a 10-minute song needs 4 minutes, not 5
    CHECK(ShouldScrobble(600000, 240));
    CHECK_FALSE(ShouldScrobble(600000, 239));
}

TEST_CASE("Last.fm's signature is the sorted names and values, then the secret") {
    const Params params = {{"method", "auth.getToken"}, {"api_key", "key"}, {"format", "json"}};
    // format is left out
    CHECK(LastfmSignature(params, "secret") == Md5Hex("api_keykeymethodauth.getTokensecret"));
}

TEST_CASE("Last.fm's form is signed, asks for JSON and is percent-encoded") {
    const Track track = Song("Won't Get Fooled Again", "The Who", 512000, 1700000000);
    const std::string form = LastfmForm(LastfmNowPlaying(track, "key", "sk1"), "secret");
    const std::string sig = Md5Hex(
        "api_keykeyartistThe Whoduration512methodtrack.updateNowPlayingsksk1"
        "trackWon't Get Fooled Againsecret");
    CHECK(form ==
          "api_key=key&api_sig=" + sig +
              "&artist=The%20Who&duration=512&format=json&method=track.updateNowPlaying&sk=sk1"
              "&track=Won%27t%20Get%20Fooled%20Again");
}

TEST_CASE("Last.fm's scrobble numbers each listen, at most 50") {
    std::vector<Track> tracks;
    for (int i = 0; i < 60; i++) tracks.push_back(Song("T" + std::to_string(i), "A", 0, 1000 + i));
    const Params params = LastfmScrobble(tracks, "key", "sk1");
    CHECK(params.at("method") == "track.scrobble");
    CHECK(params.at("artist[0]") == "A");
    CHECK(params.at("track[49]") == "T49");
    CHECK(params.at("timestamp[49]") == "1049");
    CHECK(params.count("track[50]") == 0);
    // no length, no duration
    CHECK(params.count("duration[0]") == 0);
}

TEST_CASE("Last.fm's auth page carries the key and the token") {
    CHECK(LastfmAuthUrl("key", "tok") == "https://www.last.fm/api/auth/?api_key=key&token=tok");
}

TEST_CASE("Last.fm's replies") {
    CHECK(LastfmOutcome(200, R"({"scrobbles":{"@attr":{"accepted":1}}})") == Outcome::kOk);
    std::string message;
    CHECK(LastfmOutcome(403, R"({"error":9,"message":"Invalid session key"})", &message) ==
          Outcome::kBadCredentials);
    CHECK(message == "Invalid session key");
    CHECK(LastfmOutcome(400, R"({"error":14,"message":"Unauthorized Token"})") ==
          Outcome::kNotAuthorized);
    CHECK(LastfmOutcome(503, R"({"error":16,"message":"try again"})") == Outcome::kRetry);
    CHECK(LastfmOutcome(429, R"({"error":29})") == Outcome::kRetry);
    CHECK(LastfmOutcome(0, "") == Outcome::kRetry);
    CHECK(LastfmOutcome(502, "<html>bad gateway</html>") == Outcome::kRetry);
    CHECK(LastfmOutcome(400, R"({"error":6,"message":"Invalid parameters"})") ==
          Outcome::kRejected);

    CHECK(LastfmToken(R"({"token":"abc123"})") == "abc123");
    CHECK(LastfmSessionKey(R"({"session":{"name":"user","key":"sk9","subscriber":0}})") == "sk9");
    CHECK(LastfmSessionKey(R"({"error":14})").empty());
}

TEST_CASE("ListenBrainz's listens") {
    const Track tracks[] = {Song("Say \"Hi\"", "Band", 180000, 1700000000),
                            Song("Second", "Band", 0, 1700000300)};

    const auto single = band3::json::Parse(ListenBrainzJson("single", std::span(tracks, 1)));
    REQUIRE(single);
    CHECK((*single)["listen_type"].Text() == "single");
    const auto& listen = (*single)["payload"].Items().at(0);
    CHECK(listen["listened_at"].Number() == 1700000000);
    CHECK(listen["track_metadata"]["track_name"].Text() == "Say \"Hi\"");
    CHECK(listen["track_metadata"]["artist_name"].Text() == "Band");
    CHECK(listen["track_metadata"]["additional_info"]["duration_ms"].Number() == 180000);
    CHECK(listen["track_metadata"]["additional_info"]["submission_client"].Text() ==
          "band3_recomp");

    // playing_now has no time
    const auto now = band3::json::Parse(ListenBrainzJson("playing_now", std::span(tracks, 1)));
    REQUIRE(now);
    CHECK((*now)["payload"].Items().at(0)["listened_at"].IsNull());

    const auto import = band3::json::Parse(ListenBrainzJson("import", tracks));
    REQUIRE(import);
    CHECK((*import)["payload"].Items().size() == 2);
    // no length, no duration
    CHECK((*import)["payload"].Items().at(1)["track_metadata"]["additional_info"]["duration_ms"]
              .IsNull());

    CHECK(ListenBrainzOutcome(200) == Outcome::kOk);
    CHECK(ListenBrainzOutcome(401) == Outcome::kBadCredentials);
    CHECK(ListenBrainzOutcome(429) == Outcome::kRetry);
    CHECK(ListenBrainzOutcome(0) == Outcome::kRetry);
    CHECK(ListenBrainzOutcome(400) == Outcome::kRejected);
}
