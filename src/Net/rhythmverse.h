#pragma once
#include <cstdint>
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
// songs a search page asks for
inline constexpr int kPageSize = 25;

// what a search asks RhythmVerse: a POST of a form to a URL. Without text,
// the newest songs.
struct SearchRequest {
    std::string url;
    std::string form;  // application/x-www-form-urlencoded
};
SearchRequest Search(std::string_view text, int page);

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
    // set only for files RhythmVerse hosts itself, unpacked: the ones band3
    // can download. The rest are on another site (`host`), zipped, or the
    // official DLC's store page.
    std::string download_url;
    std::string host;  // the other site's name, e.g. www.mediafire.com
};

struct SearchResult {
    int64_t total = 0;  // songs matching, over every page
    int32_t page = 1;
    std::vector<Song> songs;
};

// nullopt unless `json` is a successful reply to Search (one that found
// nothing has "songs": false)
std::optional<SearchResult> ParseSearch(std::string_view json);

// RhythmVerse's custom_id as the game's song ID: a number as it is, text as
// band3 corrects a text song_id; 0 for none, or a number no song ID can be
int32_t SongIdOf(std::string_view custom_id);

// file IDs are letters, digits and dots, which keeps them safe in a file name
bool ValidFileId(std::string_view file_id);

// the file a download is saved as: the uploaded name with anything but
// letters, digits, spaces and -_.() made '_', then '_' and the file ID
std::string DownloadFileName(const Song& song);
// the file ID of a DownloadFileName name; empty for any other name
std::string FileIdOf(std::string_view file_name);

struct Download {
    enum class State { kQueued, kDownloading, kDone, kFailed };
    std::string file_id;
    std::string title;
    std::string artist;
    State state = State::kQueued;
    int64_t received = 0;
    int64_t total = 0;  // 0 until known
    std::string error;  // kFailed's reason
};

// The songs already here, to tell a search's songs by: the files in the
// content folders, however they got there, and the songs the game has.
struct LocalSongs {
    // the files' names, ASCII lower-cased, and sizes
    std::set<std::pair<std::string, int64_t>> files;
    // the song IDs of the songs in the game; nullopt when the game couldn't say
    std::optional<std::set<int32_t>> game_ids;
};

// ASCII letters lower-cased, as LocalSongs keeps file names
std::string LowerAscii(std::string_view text);

// the song's file is in the content folders: one band3 downloaded (named by
// DownloadFileName), or one with the name RhythmVerse gives it and its size
bool IsDownloaded(const Song& song, const LocalSongs& local);

// /rv/search: {"total":, "page":, "page_size":, "songs": [{"file_id":,
// "title":, ..., "tiers": {...}, "download": true when band3 can download it,
// "downloaded": true when IsDownloaded, "in_library": true when the game has
// a song with its song ID (null when the game couldn't say)}, ...]}
std::string FormatSearch(const SearchResult& result, const LocalSongs& local);

// /rv/downloads: {"folder": where they go, "downloads": [{"file_id":, "title":,
// "artist":, "state": "queued"|"downloading"|"done"|"failed", "received":,
// "total":, "error":}, ...]}
std::string FormatDownloads(const std::vector<Download>& downloads, std::string_view folder);

}
