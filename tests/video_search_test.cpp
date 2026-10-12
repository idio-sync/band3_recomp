// Checks matching songs to their music videos' search results
// (src/Video/video_search.cpp).

#include <doctest/doctest.h>
#include <algorithm>
#include <string>
#include <vector>
#include "src/Video/video_job_state.h"
#include "src/Video/video_search.h"

using namespace band3::video;

namespace {

YtCandidate Video(std::string id, std::string title, std::string channel, double duration,
                  int64_t views = 1000000, std::string description = {}) {
    YtCandidate c;
    c.id = std::move(id);
    c.url = "https://www.youtube.com/watch?v=" + c.id;
    c.title = std::move(title);
    c.channel = std::move(channel);
    c.duration = duration;
    c.views = views;
    c.description = std::move(description);
    return c;
}

const SongInfo kPuppets{"masterofpuppets", "Master of Puppets", "Metallica", 519095};

const ScoredCandidate& Find(const std::vector<ScoredCandidate>& ranked, std::string_view id) {
    return *std::find_if(ranked.begin(), ranked.end(),
                         [&](const ScoredCandidate& s) { return s.video.id == id; });
}

}

TEST_CASE("titles are compared by their words, without case, accents or punctuation") {
    CHECK(Normalized("Metallica: Master of Puppets (Official Music Video)") ==
          "metallica master of puppets official music video");
    CHECK(Normalized("Beyonc\xC3\xA9 \xE2\x80\x93 D\xC3\xA9j\xC3\xA0 Vu") == "beyonce deja vu");
    CHECK(Normalized("Guns N' Roses - Sweet Child O' Mine") == "guns n roses sweet child o mine");
    CHECK(Normalized("AC/DC & Friends") == "ac dc and friends");
    CHECK(Normalized("  ") == "");
}

TEST_CASE("a chart's title loses what charts add to it, not what the song's called") {
    CHECK(ChartTitle("Master of Puppets (2x Bass Pedal)") == "Master of Puppets");
    CHECK(ChartTitle("Through the Fire and Flames (Rerecord)") == "Through the Fire and Flames");
    CHECK(ChartTitle("Hysteria [RB3 Version]") == "Hysteria");
    CHECK(ChartTitle("Paranoid (feat. Someone)") == "Paranoid");
    CHECK(ChartTitle("Crazy Train (Album Version)") == "Crazy Train");
    // part of the song's name
    CHECK(ChartTitle("(Don't Fear) The Reaper") == "(Don't Fear) The Reaper");
    CHECK(ChartTitle("Hey Jude (Live)") == "Hey Jude (Live)");
}

TEST_CASE("a song's search asks for its official video by its artist and title") {
    CHECK(SearchQuery(kPuppets) == "Metallica Master of Puppets official music video");
    CHECK(PlainQuery({"x", "Master of Puppets (2x Bass Pedal)", "Metallica", 0}) ==
          "Metallica Master of Puppets");
}

TEST_CASE("the official video ranks first; lyric, live, cover and art tracks don't pass") {
    const std::vector<YtCandidate> results = {
        Video("lyric", "Metallica - Master of Puppets (Lyrics)", "Lyric Vault", 515, 9000000),
        Video("offic", "Metallica: Master of Puppets (Official Music Video)", "Metallica", 517,
              120000000),
        Video("topic", "Master of Puppets (Remastered)", "Metallica - Topic", 516, 50000000,
              "Provided to YouTube by Universal Music Group"),
        Video("live", "Metallica - Master of Puppets (Live in Seattle 1989)", "Metallica", 560),
        Video("cover", "Master of Puppets - Guitar Cover", "Shreds McGee", 480, 40000),
        Video("other", "Metallica - Enter Sandman (Official Music Video)", "Metallica", 330),
    };
    const auto ranked = Rank(kPuppets, results);
    REQUIRE(ranked.size() == results.size());
    CHECK(ranked[0].video.id == "offic");
    CHECK(CanAutoAccept(ranked[0]));
    CHECK(Find(ranked, "lyric").penalized);
    CHECK_FALSE(CanAutoAccept(Find(ranked, "lyric")));
    CHECK(Find(ranked, "topic").rejected);
    CHECK_FALSE(CanAutoAccept(Find(ranked, "topic")));
    CHECK(Find(ranked, "live").penalized);
    CHECK(Find(ranked, "cover").penalized);
    // another song by the same band isn't it
    CHECK(Find(ranked, "other").score < kPlausibleScore);
    CHECK_FALSE(Find(ranked, "lyric").note.empty());
    CHECK_FALSE(Find(ranked, "topic").note.empty());
}

TEST_CASE("a word in the chart's own title isn't held against a video") {
    const SongInfo live{"heyjudelive", "Hey Jude (Live)", "The Beatles", 420000};
    const auto s = Score(live, Video("a", "The Beatles - Hey Jude (Live at Shea)", "The Beatles", 430));
    CHECK_FALSE(s.penalized);
    const SongInfo acoustic{"x", "Layla (Acoustic)", "Eric Clapton", 280000};
    CHECK_FALSE(Score(acoustic, Video("b", "Eric Clapton - Layla (Acoustic) [Official Video]",
                                      "Eric Clapton", 290))
                    .penalized);
}

TEST_CASE("a video much shorter than the song scores less; a longer one is usual") {
    const auto right = Score(kPuppets, Video("a", "Metallica - Master of Puppets", "Metallica", 520));
    const auto longer = Score(kPuppets, Video("b", "Metallica - Master of Puppets", "Metallica", 580));
    const auto edit = Score(kPuppets, Video("c", "Metallica - Master of Puppets", "Metallica", 300));
    CHECK(longer.score > right.score - 0.01);
    CHECK(edit.score < right.score - 0.1);
    // not known: no matter
    const auto unknown = Score({"x", "Master of Puppets", "Metallica", 0},
                               Video("d", "Metallica - Master of Puppets", "Metallica", 520));
    CHECK(unknown.score > 0.5);
}

TEST_CASE("what each song's search came to is saved and read back") {
    SongVideoStatuses statuses;
    SongVideoStatus& s = statuses["masterofpuppets"];
    s.end = FetchEnd::kReview;
    s.message = "nothing sure \"enough\"";
    s.when = 1791800000;
    TriedCandidate t;
    t.candidate = Score(kPuppets, Video("offic", "Metallica: Master of Puppets", "Metallica", 517.5));
    t.problem = "its sound doesn't surely match";
    SyncResult r;
    r.found = true;
    r.offset = -3.252;
    r.score = 0.81;
    r.margin = 1.2;
    t.match = r;
    s.candidates.push_back(t);
    TriedCandidate lyric;
    lyric.candidate = Score(kPuppets, Video("lyr", "Master of Puppets (Lyrics)", "X", 515));
    s.candidates.push_back(lyric);
    statuses["other"].end = FetchEnd::kNoVideo;

    const SongVideoStatuses back = ParseVideoStatuses(FormatVideoStatuses(statuses));
    REQUIRE(back.size() == 2);
    const SongVideoStatus& b = back.at("masterofpuppets");
    CHECK(b.end == FetchEnd::kReview);
    CHECK(b.message == s.message);
    CHECK(b.when == 1791800000);
    REQUIRE(b.candidates.size() == 2);
    CHECK(b.candidates[0].candidate.video.url == "https://www.youtube.com/watch?v=offic");
    CHECK(b.candidates[0].candidate.video.duration == 517.5);
    CHECK(b.candidates[0].candidate.score == doctest::Approx(t.candidate.score));
    REQUIRE(b.candidates[0].match);
    CHECK(b.candidates[0].match->offset == doctest::Approx(-3.252));
    CHECK_FALSE(b.candidates[0].match->confident);
    CHECK(b.candidates[1].candidate.penalized);
    CHECK_FALSE(b.candidates[1].match);
    CHECK(back.at("other").end == FetchEnd::kNoVideo);
    CHECK(ParseVideoStatuses("").empty());
    CHECK(ParseVideoStatuses(R"({"version": 2, "songs": {"x": {"end": "done"}}})").empty());
}

TEST_CASE("an artist's own channel or VEVO counts for it") {
    const SongInfo song{"x", "Take On Me", "a-ha", 225000};
    const auto vevo = Score(song, Video("a", "Take On Me", "ahaVEVO", 228));
    const auto own = Score(song, Video("b", "Take On Me (Official Video)", "a-ha", 228));
    const auto stranger = Score(song, Video("c", "Take On Me", "Random 80s", 228));
    CHECK(vevo.score > stranger.score);
    CHECK(own.score > stranger.score);
    CHECK(CanAutoAccept(own));
}
