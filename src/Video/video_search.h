#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "src/Video/ytdlp.h"

// Matching a song to its music video among a search's results, by what
// yt-dlp says of each: its title has the song's title and artist, its
// channel is the artist's own (or VEVO), "official video" in its title, its
// length near the song's. Words that say it's another kind of upload (a
// lyric video, a live one, a cover...) count against it, unless the chart's
// own title has them; an auto-generated art track (a "- Topic" channel) is
// never taken without asking, as its sound matches the song perfectly over a
// still picture. The score only orders which results' sound is fetched to
// match against the song (video_fetch.h): the match decides.

namespace band3::video {

struct SongInfo {
    std::string shortname;
    std::string title;
    std::string artist;
    int length_ms = 0;  // 0 not known
};

// lower case, accents taken off Latin letters, & as "and", apostrophes
// dropped, other punctuation as spaces, one space between words
std::string Normalized(std::string_view text);

// a chart's title without what charts add in brackets ("(2x Bass Pedal)",
// "[RB3 Version]", "(feat. ...)"), kept where the brackets are the song's
// own ("(Don't Fear) The Reaper") or say what kind of take it is ("(Live)")
std::string ChartTitle(std::string_view title);

// "<artist> <title> official music video", and without the last words
std::string SearchQuery(const SongInfo& song);
std::string PlainQuery(const SongInfo& song);

struct ScoredCandidate {
    YtCandidate video;
    double score = 0.0;      // about 0..1
    bool penalized = false;  // a word says it's another kind of upload
    bool rejected = false;   // an art track: never taken without asking
    std::string note;        // why, for the Music Videos screen; "" for neither
};

ScoredCandidate Score(const SongInfo& song, const YtCandidate& video);
// all of them, best first
std::vector<ScoredCandidate> Rank(const SongInfo& song, const std::vector<YtCandidate>& videos);

// worth fetching the sound of, to match against the song
inline constexpr double kPlausibleScore = 0.35;
// taken without asking, once its sound matches too
inline constexpr double kAutoAcceptScore = 0.6;
bool CanAutoAccept(const ScoredCandidate& c);

}
