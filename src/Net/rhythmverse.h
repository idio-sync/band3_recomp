#pragma once
#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// RhythmVerse (rhythmverse.co), the custom song site, as the web page's
// RhythmVerse tab uses it: its search, and the songs it hosts itself, which
// download as the CON packages band3 reads from its content folders. The parts
// here need neither a network nor the game; song_downloads.h does the rest.
// RhythmVerse's API isn't documented: this is what its own pages send and get.

namespace band3::rhythmverse {

inline constexpr std::string_view kSite = "https://rhythmverse.co";
// songs a search page asks for; for downloadable ones only, more, since band3
// leaves out the rest
inline constexpr int kPageSize = 25;
inline constexpr int kDownloadablePageSize = 100;

// what the page's RhythmVerse tab asks for (/rv/search's query: text=, page=,
// sort=, downloadable=1, has=keys,real_keys, harmonies=1, genre=metal,rock,
// decade=1990,2000, cap=drum:3). Parts are the game's names for them, as in
// /song_details; values RhythmVerse doesn't have are left out.
struct SearchOptions {
    std::string text;
    int32_t page = 1;
    // newest, updated, downloads, title, artist or length; empty for the
    // search's own order, or the newest without text
    std::string sort;
    bool downloadable_only = false;
    std::vector<std::string> has;  // parts the songs have
    bool harmonies = false;        // two or three vocal parts
    std::vector<std::string> genres;  // RhythmVerse's: rock, poprock, metal...
    std::vector<int32_t> decades;     // 1990, 2000...
    // a part's difficulty at most: the game's tier, 0 (Warmup) to 6
    std::string cap_part;
    int32_t cap_tier = -1;
};
SearchOptions ParseSearchOptions(std::string_view target);

// what a search asks RhythmVerse: a POST of a form to a URL
struct SearchRequest {
    std::string url;
    std::string form;  // application/x-www-form-urlencoded
    int32_t page_size = kPageSize;
};
SearchRequest Search(const SearchOptions& options);

// text with everything but letters, digits and -._~ percent-encoded
std::string FormEncode(std::string_view text);

struct Song {
    std::string file_id;  // RhythmVerse's for this upload, e.g. 595481a7cbc158.68319817
    std::string title;
    std::string artist;
    std::string album;
    std::string genre;
    std::string author;
    int32_t year = 0;
    int32_t length_s = 0;
    int32_t vocal_parts = 0;
    int64_t size = 0;  // bytes, 0 when unknown
    int64_t downloads = 0;
    // the game's difficulty tier, 0 (Warmup) to 6 (Impossible), of each part
    // the song has, by the game's names for them, as /song_details gives them
    std::vector<std::pair<std::string, int32_t>> tiers;
    std::string art_url;   // absolute; empty when there's none
    std::string page_url;  // the upload's page on RhythmVerse
    std::string file_name;  // as uploaded, and as RhythmVerse's own downloads name it
    // the song_id in the package's songs.dta (RhythmVerse's custom_id), as the
    // game has it: a text one as band3 turns it into a number (CorrectedSongId);
    // 0 when RhythmVerse doesn't say
    int32_t song_id = 0;
    // RhythmVerse's hashes of the upload's files, which change with them; empty
    // when it gives none
    std::string hash;
    // set only for files RhythmVerse hosts itself, unpacked: the ones band3
    // can download. The rest are on another site (`host`), zipped, or the
    // official DLC's store page.
    std::string download_url;
    std::string host;  // the other site's name, e.g. www.mediafire.com
    // when RhythmVerse last changed the upload (its update_date), as RvTime
    // reads it; 0 when it doesn't say
    int64_t updated = 0;
};

struct SearchResult {
    int64_t total = 0;  // songs matching, over every page
    int32_t page = 1;
    int32_t page_size = kPageSize;
    std::vector<Song> songs;
};

// nullopt unless `json` is a successful reply to Search (one that found
// nothing has "songs": false)
std::optional<SearchResult> ParseSearch(std::string_view json);

// RhythmVerse's "2026-10-06 13:54:57" as seconds since 1970, read as UTC (its
// own time zone isn't known, so its times are only compared, with a margin);
// nullopt for anything else, its "0000-00-00 00:00:00" too
std::optional<int64_t> RvTime(std::string_view text);

// RhythmVerse's custom_id as the game's song ID: a number as it is, text as
// band3 corrects a text song_id; 0 for none, or a number no song ID can be
int32_t SongIdOf(std::string_view custom_id);

// file IDs are letters, digits and dots, which keeps them safe in a file name
bool ValidFileId(std::string_view file_id);

// the file a download is saved as: the uploaded name with anything but
// letters, digits, spaces and -_.() made '_', then '_' and the file ID
std::string DownloadFileName(const Song& song);

struct Download {
    enum class State { kQueued, kDownloading, kDone, kFailed };
    std::string file_id;
    std::string title;
    std::string artist;
    int32_t song_id = 0;  // Song's
    bool update = false;  // a newer version of one band3 downloaded, for the next launch
    State state = State::kQueued;
    int64_t received = 0;
    int64_t total = 0;  // 0 until known
    std::string error;  // kFailed's reason
};

// What band3 downloaded of an upload, kept in the download folder's
// rhythmverse.json, to tell when RhythmVerse has a newer version of it
struct DownloadRecord {
    std::string file_name;     // the file it's in, in the download folder
    std::string hash;          // Song::hash, as downloaded
    std::string pending_hash;  // an update's, waiting for the next launch (file_name + .pending)
    // when band3 downloaded it, in seconds since 1970; 0 for a record from
    // before band3 kept it, until the file's time stands in for it
    int64_t downloaded = 0;
    // the upload as RhythmVerse last showed it, in a search or an update
    // check, to tell of an update without searching for the song; nullopt for
    // a record from before band3 kept it, until RhythmVerse shows it again
    std::optional<Song> latest;
};
// by file ID
using DownloadRecords = std::map<std::string, DownloadRecord>;

// How far the last update check read back through RhythmVerse's uploads, the
// most recently updated first
struct CheckState {
    int64_t through = 0;  // the newest upload's Song::updated it saw; 0 before the first
    int64_t at = 0;       // when it finished, in seconds since 1970; 0 before the first
};

// {"<file_id>": {"file": , "hash": , "pending": , "downloaded": , "latest":
// {the song}}, ..., "_check": {"through": , "at": }}; records it can't read
// are left out. "_check" isn't a file ID, so a band3 from before it skips it.
DownloadRecords ParseRecords(std::string_view json);
CheckState ParseCheckState(std::string_view json);
std::string FormatRecords(const DownloadRecords& records, const CheckState& check = {});

// what's become of an upload band3 downloaded
enum class UpdateState {
    kNone,       // not downloaded by band3, the same as RhythmVerse's, or no hash to tell
    kAvailable,  // RhythmVerse has another version, which band3 can download
    kPending,    // the latest is downloaded, for the next launch
};
UpdateState UpdateOf(const Song& song, const DownloadRecords& records);
// as UpdateOf, for the latest version the upload's record has seen
UpdateState RecordUpdate(std::string_view file_id, const DownloadRecords& records);

// Songs a search or an update check found: the records of those band3
// downloaded take them as their latest. True when that changed a record's
// version (its hash or download), so the records are worth writing.
bool NoteLatest(DownloadRecords& records, const std::vector<Song>& songs);

// The update check reads RhythmVerse's uploads the most recently updated
// first, a page at a time, back to FeedStop: one or two pages for a check a
// day or so after the last, without asking about each song.
inline constexpr int64_t kFeedMargin = 24 * 60 * 60;
SearchRequest UpdateFeed(int32_t page);
// The last check's newest upload, or the oldest download if that's later
// (nothing older can be newer than what was downloaded), less kFeedMargin:
// RhythmVerse's times are in a time zone of its own, and its order by them
// isn't quite exact. nullopt with no records, so nothing to check.
std::optional<int64_t> FeedStop(const DownloadRecords& records, const CheckState& check);
// a feed page whose uploads were all updated before stop (or an empty one) is
// the last to read
bool FeedReachedBack(const std::vector<Song>& page, int64_t stop);

// The songs already here, to tell a search's songs by: the files in the
// content folders, however they got there, and the songs the game has.
struct LocalSongs {
    // the files' names, ASCII lower-cased, and sizes
    std::set<std::pair<std::string, int64_t>> files;
    // the song IDs of the songs in the game; nullopt when the game couldn't say
    std::optional<std::set<int32_t>> game_ids;
    DownloadRecords records;
};

// ASCII letters lower-cased, as LocalSongs keeps file names
std::string LowerAscii(std::string_view text);

// the song's file is in the content folders: one band3 downloaded (named by
// DownloadFileName, or as its record says), or one with the name RhythmVerse
// gives it and its size
bool IsDownloaded(const Song& song, const LocalSongs& local);

// /rv/search: {"total":, "page":, "page_size":, "songs": [{"file_id":,
// "title":, ..., "tiers": {...}, "download": true when band3 can download it,
// "downloaded": true when IsDownloaded, "song_id": as the game has it (0 for
// none), "in_library": true when the game has a song with its song ID (null
// when the game couldn't say), "update": "available" or "pending" (UpdateOf)
// or ""}, ...]}
std::string FormatSearch(const SearchResult& result, const LocalSongs& local);

// /rv/downloads: {"folder": where they go, "downloads": [{"file_id":, "title":,
// "artist":, "state": "queued"|"downloading"|"done"|"failed", "received":,
// "total":, "error":, "song_id":, "update":, "in_library": the game has its
// song ID (null when game_ids is)}, ...]}
std::string FormatDownloads(const std::vector<Download>& downloads, std::string_view folder,
                            const std::optional<std::set<int32_t>>& game_ids);

struct UpdateCheckStatus {
    bool checking = false;
    int64_t checked = 0;  // CheckState::at
    std::string error;    // why the last check failed; "" when it didn't
};

// /rv/updates: {"checking":, "checked": seconds since 1970 (0 for never),
// "error":, "downloads": how many uploads band3 has records of, "updates":
// [the songs RhythmVerse has newer versions of, as /rv/search gives them, with
// "update" "available", or "pending" once downloaded], by artist then title}
std::string FormatUpdates(const LocalSongs& local, const UpdateCheckStatus& status);

}
