#pragma once
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Score history: every finished song's results, one run per player, kept in
// score_history.jsonl in the user data folder (one JSON object a line,
// appended, never rewritten) and indexed in memory for the web page's
// /history endpoints. src/Hooks/score_history.cpp fills it from
// MetaPerformer::TriggerSongCompletion. None of this needs the game, so the
// unit tests drive it directly.

namespace band3::history {

// One player's results for one finished song.
struct Run {
    int64_t time = 0;  // Unix seconds, UTC
    std::string shortname;
    int32_t song_id = 0;
    std::string title;
    std::string artist;
    // RB3's ScoreType as a name: drum, bass, guitar, vocals, harmony, keys,
    // real_drum, real_guitar, real_bass, real_keys
    std::string part;
    int diff = 0;    // 0 easy .. 3 expert
    int score = 0;
    int stars = 0;   // 0-6, 6 being gold
    int pct = 0;     // notes hit, 0-100
    int streak = 0;  // the longest
    int od_deploys = 0;
    std::optional<int> solo_pct;  // only for a part whose song has solos
    std::optional<int> unison;    // unison phrases completed, in a band
    int band_size = 1;
    int speed = 100;  // the song's speed, in percent
    std::vector<std::string> modifiers;
    bool autoplay = false;  // only ever stored by a harness session

    // a run that counts toward bests: no modifiers, at full speed, not autoplay
    bool Counts() const;
};

// the schema version this build writes; lines with a newer one are skipped
inline constexpr int kVersion = 1;

// a run as one line of score_history.jsonl, without the newline
std::string FormatLine(const Run& run);
// nullopt for a line that isn't a run (torn, garbled, or a newer version)
std::optional<Run> ParseLine(std::string_view line);

// whether a run read from the game is worth keeping: in range everywhere
bool Plausible(const Run& run);

// a part + difficulty, as "real_drum:3"
std::string PartKey(std::string_view part, int diff);

struct PartSummary {
    int plays = 0;
    int64_t last = 0;
    // over runs that count; 0 while none does
    int best_score = 0;
    int best_stars = 0;
    int best_pct = 0;
};

struct SongSummary {
    int plays = 0;
    int64_t last = 0;
    std::map<std::string, PartSummary> parts;  // by PartKey
};

// The runs and their summaries. Not thread-safe; Store guards it.
class Index {
public:
    void Add(Run run);
    // lines that weren't runs, while loading
    int skipped = 0;

    const std::vector<Run>& Runs() const { return runs_; }
    const std::map<std::string, SongSummary, std::less<>>& Songs() const { return songs_; }

private:
    std::vector<Run> runs_;  // oldest first
    std::map<std::string, SongSummary, std::less<>> songs_;
};

// reads score_history.jsonl's text into an index, skipping (and counting)
// lines that aren't runs
Index ParseFile(std::string_view text);

// the /history endpoints' JSON
std::string SummaryJson(const Index& index);
// a song's runs, newest first; part empty for every part, diff < 0 for every
// difficulty
std::string RunsJson(const Index& index, std::string_view shortname, std::string_view part,
                     int diff);
// the last `limit` runs across all songs, newest first
std::string RecentJson(const Index& index, size_t limit);
// what every endpoint answers with history off
std::string DisabledJson();

// The history file and its index. Append hands the line to a writer thread,
// so a hook never waits on the disk.
class Store {
public:
    // loads `path` (missing is fine: an empty history)
    explicit Store(std::filesystem::path path);
    ~Store();
    Store(const Store&) = delete;
    Store& operator=(const Store&) = delete;

    void Append(Run run);
    // waits until every appended run is on disk
    void Flush();

    std::string Summary() const;
    std::string Runs(std::string_view shortname, std::string_view part, int diff) const;
    std::string Recent(size_t limit) const;

    const std::filesystem::path& Path() const { return path_; }

private:
    struct Impl;
    std::filesystem::path path_;
    Impl* impl_;
};

// The game's store: Open once the settings and folders are known (a no-op
// when score_history is off and no harness file is set), then Get from
// anywhere; null while closed.
void Open(const std::filesystem::path& path);
Store* Get();

// Opens score_history.jsonl in the user data folder, or a harness session's
// test_score_history file (src/Hooks/score_history.cpp, which needs the game)
void OpenForGame();

}
