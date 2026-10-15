// Checks score history's store (src/History/score_history.cpp): the lines of
// score_history.jsonl, the summaries the web page filters on, and the
// /history endpoints' JSON.

#include <doctest/doctest.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include "src/History/score_history.h"
#include "src/Net/json.h"

using namespace band3::history;

namespace {

Run MakeRun(std::string shortname, std::string part, int diff, int score, int64_t time) {
    Run run;
    run.time = time;
    run.shortname = std::move(shortname);
    run.song_id = 1234;
    run.title = "20th Century Boy";
    run.artist = "T. Rex";
    run.part = std::move(part);
    run.diff = diff;
    run.score = score;
    run.stars = 5;
    run.pct = 95;
    run.streak = 300;
    run.od_deploys = 3;
    return run;
}

std::string ReadAll(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

}  // namespace

TEST_CASE("a run survives its line") {
    Run run = MakeRun("20thcenturyboy", "real_drum", 3, 184320, 1760475791);
    run.solo_pct = 100;
    run.unison = 3;
    run.band_size = 2;
    run.speed = 80;
    run.modifiers = {"no_fail"};
    const std::string line = FormatLine(run);
    CHECK(line.find('\n') == std::string::npos);
    const auto back = ParseLine(line);
    REQUIRE(back);
    CHECK(back->time == run.time);
    CHECK(back->shortname == run.shortname);
    CHECK(back->song_id == 1234);
    CHECK(back->title == run.title);
    CHECK(back->part == "real_drum");
    CHECK(back->diff == 3);
    CHECK(back->score == 184320);
    CHECK(back->stars == 5);
    CHECK(back->pct == 95);
    CHECK(back->streak == 300);
    CHECK(back->od_deploys == 3);
    CHECK(back->solo_pct == 100);
    CHECK(back->unison == 3);
    CHECK(back->band_size == 2);
    CHECK(back->speed == 80);
    REQUIRE(back->modifiers.size() == 1);
    CHECK(back->modifiers[0] == "no_fail");
    CHECK_FALSE(back->autoplay);
}

TEST_CASE("fields a part doesn't have are left out, not zeroed") {
    const Run run = MakeRun("song", "vocals", 2, 1000, 1);
    const std::string line = FormatLine(run);
    CHECK(line.find("solo_pct") == std::string::npos);
    CHECK(line.find("unison") == std::string::npos);
    CHECK(line.find("autoplay") == std::string::npos);
    const auto back = ParseLine(line);
    REQUIRE(back);
    CHECK_FALSE(back->solo_pct);
    CHECK_FALSE(back->unison);
}

TEST_CASE("only unmodified full-speed runs count toward bests") {
    Run run = MakeRun("song", "guitar", 3, 1000, 1);
    CHECK(run.Counts());
    run.speed = 80;
    CHECK_FALSE(run.Counts());
    run.speed = 100;
    run.modifiers = {"no_fail"};
    CHECK_FALSE(run.Counts());
    run.modifiers.clear();
    run.autoplay = true;
    CHECK_FALSE(run.Counts());
}

TEST_CASE("the index sums plays and keeps bests from counted runs only") {
    Index index;
    index.Add(MakeRun("a", "guitar", 3, 1000, 10));
    Run slow = MakeRun("a", "guitar", 3, 5000, 30);
    slow.speed = 70;
    slow.stars = 6;
    index.Add(slow);
    Run better = MakeRun("a", "guitar", 3, 2000, 20);
    better.pct = 99;
    index.Add(better);
    index.Add(MakeRun("a", "drum", 1, 700, 25));
    index.Add(MakeRun("b", "bass", 2, 300, 5));

    REQUIRE(index.Songs().size() == 2);
    const SongSummary& a = index.Songs().at("a");
    CHECK(a.plays == 4);
    CHECK(a.last == 30);
    const PartSummary& guitar = a.parts.at(PartKey("guitar", 3));
    CHECK(guitar.plays == 3);
    CHECK(guitar.last == 30);
    CHECK(guitar.best_score == 2000);  // not the slowed run's 5000
    CHECK(guitar.best_stars == 5);     // nor its gold
    CHECK(guitar.best_pct == 99);
    CHECK(a.parts.at(PartKey("drum", 1)).plays == 1);
}

TEST_CASE("a part played only modified has plays but no bests") {
    Index index;
    Run run = MakeRun("a", "keys", 3, 1000, 1);
    run.modifiers = {"no_fail"};
    index.Add(run);
    const PartSummary& keys = index.Songs().at("a").parts.at(PartKey("keys", 3));
    CHECK(keys.plays == 1);
    CHECK(keys.best_score == 0);
    CHECK(keys.best_stars == 0);
}

TEST_CASE("torn, garbled and newer lines are skipped and counted") {
    const std::string good = FormatLine(MakeRun("a", "guitar", 3, 1000, 1));
    std::string newer = good;
    newer.replace(newer.find("\"v\":1"), 5, "\"v\":2");
    const std::string text = good + "\r\n" + "not json\n" + newer + "\n\n" + good + "\n" +
                             good.substr(0, good.size() / 2);
    const Index index = ParseFile(text);
    CHECK(index.Runs().size() == 2);
    CHECK(index.skipped == 3);
}

TEST_CASE("implausible readings are rejected") {
    const Run ok = MakeRun("a", "guitar", 3, 1000, 1);
    CHECK(Plausible(ok));
    Run bad = ok;
    bad.stars = 7;
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.pct = 101;
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.score = -1;
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.diff = 4;
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.part = "kazoo";
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.shortname.clear();
    CHECK_FALSE(Plausible(bad));
    bad = ok;
    bad.solo_pct = 150;
    CHECK_FALSE(Plausible(bad));
}

TEST_CASE("the summary JSON has each song's parts and bests") {
    Index index;
    index.Add(MakeRun("a", "real_drum", 3, 1000, 10));
    const auto v = band3::json::Parse(SummaryJson(index));
    REQUIRE(v);
    CHECK((*v)["enabled"].Bool());
    const auto& part = (*v)["songs"]["a"]["parts"]["real_drum:3"];
    CHECK(part["plays"].Number() == 1);
    CHECK(part["best_score"].Number() == 1000);
    CHECK(part["best_stars"].Number() == 5);
    CHECK(part["last"].Number() == 10);
    CHECK((*v)["songs"]["a"]["plays"].Number() == 1);
}

TEST_CASE("a song's runs come newest first, filtered by part and difficulty") {
    Index index;
    index.Add(MakeRun("a", "guitar", 3, 1, 1));
    index.Add(MakeRun("a", "guitar", 2, 2, 2));
    index.Add(MakeRun("b", "guitar", 3, 3, 3));
    Run slow = MakeRun("a", "drum", 3, 4, 4);
    slow.speed = 50;
    index.Add(slow);

    auto v = band3::json::Parse(RunsJson(index, "a", "", -1));
    REQUIRE(v);
    const auto& all = (*v)["runs"].Items();
    REQUIRE(all.size() == 3);
    CHECK(all[0]["score"].Number() == 4);
    CHECK_FALSE(all[0]["counts"].Bool(true));
    CHECK(all[2]["score"].Number() == 1);
    CHECK(all[2]["counts"].Bool());

    v = band3::json::Parse(RunsJson(index, "a", "guitar", 3));
    REQUIRE(v);
    REQUIRE((*v)["runs"].Items().size() == 1);
    CHECK((*v)["runs"].Items()[0]["score"].Number() == 1);
}

TEST_CASE("recent runs span songs and stop at the limit") {
    Index index;
    for (int i = 0; i < 5; i++) index.Add(MakeRun(i % 2 ? "a" : "b", "bass", 1, i, i));
    const auto v = band3::json::Parse(RecentJson(index, 3));
    REQUIRE(v);
    const auto& runs = (*v)["runs"].Items();
    REQUIRE(runs.size() == 3);
    CHECK(runs[0]["score"].Number() == 4);
    CHECK(runs[2]["score"].Number() == 2);
}

TEST_CASE("history off answers enabled false") {
    const auto v = band3::json::Parse(DisabledJson());
    REQUIRE(v);
    CHECK_FALSE((*v)["enabled"].Bool(true));
}

TEST_CASE("titles in Latin-1 are written as UTF-8") {
    Run run = MakeRun("a", "guitar", 3, 1, 1);
    run.title = "Mot\xf6rhead";  // Latin-1, as some songs' metadata is
    const auto back = ParseLine(FormatLine(run));
    REQUIRE(back);
    CHECK(back->title == "Mot\xc3\xb6rhead");
}

TEST_CASE("the store appends to the file and reloads it") {
    const auto dir = std::filesystem::temp_directory_path() / "band3_score_history_test";
    std::filesystem::remove_all(dir);
    const auto path = dir / "score_history.jsonl";
    {
        Store store(path);  // no file yet: an empty history, and the folder made on write
        store.Append(MakeRun("a", "guitar", 3, 1000, 1));
        store.Append(MakeRun("a", "guitar", 3, 2000, 2));
        store.Flush();
        const auto v = band3::json::Parse(store.Summary());
        REQUIRE(v);
        CHECK((*v)["songs"]["a"]["plays"].Number() == 2);
    }
    const std::string text = ReadAll(path);
    CHECK(std::count(text.begin(), text.end(), '\n') == 2);
    CHECK(text.find('\r') == std::string::npos);
    {
        Store store(path);
        store.Append(MakeRun("b", "bass", 0, 10, 3));
        store.Flush();
        const auto v = band3::json::Parse(store.Summary());
        REQUIRE(v);
        CHECK((*v)["songs"]["a"]["parts"]["guitar:3"]["best_score"].Number() == 2000);
        CHECK((*v)["songs"]["b"]["plays"].Number() == 1);
    }
    std::filesystem::remove_all(dir);
}
