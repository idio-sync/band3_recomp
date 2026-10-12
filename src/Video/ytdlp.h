#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// yt-dlp, which the player supplies (music_video_ytdlp_path, else on PATH;
// band3 never ships or fetches it): searching for a song's video and
// downloading it, run as its own program (child_process.h). Its sound and
// its picture are fetched apart: the sound first, small, to match against
// the song (auto_sync.h), and the picture only once that's sure, so a wrong
// upload costs a few MB, and neither needs yt-dlp's FFmpeg to merge them
// (band3 plays the picture under the game's own sound).

namespace band3::video {

// a search result, or a link's video, as yt-dlp describes it
struct YtCandidate {
    std::string id;
    std::string url;  // its page
    std::string title;
    std::string channel;      // the channel, else the uploader
    std::string description;  // search results often have none
    double duration = 0.0;    // seconds; 0 not known
    int64_t views = -1;       // -1 not known
    bool verified = false;    // the channel's verified
};

// one line of --dump-json; none for what isn't a video's
std::optional<YtCandidate> ParseCandidate(std::string_view json_line);

// the picture wanted
struct VideoFormat {
    int max_height = 1080;
    bool prefer_60fps = true;
};

// arguments for a search of `count` results, without fetching their pages
std::vector<std::string> SearchArgs(std::string_view query, int count);
// a link's video described, not fetched
std::vector<std::string> InfoArgs(std::string_view url);
// its best sound (or, where the site has none apart, its one file), saved as
// out_stem.<ext>
std::vector<std::string> AudioArgs(std::string_view url, const std::filesystem::path& out_stem);
// its best picture up to the format's height (60 fps first if asked, H.264
// first as the lightest to decode), saved as out_stem.<ext>; with
// `whole_file`, its best single file of picture and sound together, for when
// the picture alone is refused (YouTube's 403 now and then)
std::vector<std::string> VideoArgs(std::string_view url, const std::filesystem::path& out_stem,
                                   const VideoFormat& format, bool whole_file = false);

// the player's extra flags (music_video_ytdlp_args), split at spaces, "quoted"
// parts kept whole
std::vector<std::string> SplitArgs(std::string_view text);

// YouTube needs yt-dlp to run its player's JavaScript, which it does with a
// JavaScript runtime it finds (Deno, Node.js, Bun or QuickJS); without one,
// some videos' sound and pictures are refused (HTTP 403). yt-dlp uses only
// Deno unless told otherwise: these tell it to use any of them (for a yt-dlp
// with --js-runtimes).
std::vector<std::string> JsRuntimeArgs();
// whether one of those runtimes is on PATH (`path`, separated as the system
// does)
bool JsRuntimeOnPath(std::string_view path);

// what a download prints, as the arguments above ask for it
struct Progress {
    int64_t done = 0;
    int64_t total = 0;  // 0 not known
};
std::optional<Progress> ParseProgress(std::string_view line);
// the file a download saved
std::optional<std::string> ParseSavedFile(std::string_view line);
// "ERROR: [youtube] abc: Video unavailable" as "Video unavailable"
std::optional<std::string> ParseError(std::string_view line);
// an error as the Videos tab shows it: its first sentence, and what to do
// about one that asks to sign in
std::string Briefly(std::string_view error);

// what a failure says of the machine rather than the video: the network's
// down, or the disk's full; a batch then stops rather than fail every song
// after
enum class Trouble { kNone, kNetwork, kDisk };
Trouble TroubleOf(std::string_view error);

// Runs yt-dlp. Each call waits for it (on a thread that may), and stops it
// when `cancel` is set; on failure, why in `error`, yt-dlp's own words where
// it gave some.
class YtDlp {
public:
    // `exe` "" looks for yt-dlp on PATH
    YtDlp(std::filesystem::path exe, std::vector<std::string> extra_args);

    std::optional<std::string> Version(std::string& error) const;
    // whether its --help lists `option` (one newer than some yt-dlp's)
    bool Supports(std::string_view option) const;
    std::vector<YtCandidate> Search(std::string_view query, int count, std::string& error,
                                    const std::atomic<bool>* cancel = nullptr) const;
    std::optional<YtCandidate> Info(std::string_view url, std::string& error,
                                    const std::atomic<bool>* cancel = nullptr) const;
    // runs AudioArgs or VideoArgs' arguments; the file saved
    std::optional<std::filesystem::path> Download(
        const std::vector<std::string>& args, const std::function<void(const Progress&)>& progress,
        std::string& error, const std::atomic<bool>* cancel = nullptr) const;
    // yt-dlp -U: what it said, and whether it worked
    bool Update(std::string& output) const;

    const std::filesystem::path& Exe() const { return exe_; }

private:
    // runs it with these arguments (and the player's), each line to on_line;
    // false with `error` set if it failed
    bool Run(std::vector<std::string> args, bool with_extra,
             const std::function<void(std::string_view)>& on_line, std::string& error,
             const std::atomic<bool>* cancel) const;

    std::filesystem::path exe_;
    std::vector<std::string> extra_;
};

}
