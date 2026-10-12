#include "src/Video/ytdlp.h"

#include <charconv>
#include <cstdlib>

#include "src/Net/json.h"
#include "src/Video/child_process.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace band3::video {

namespace {

constexpr std::string_view kProgress = "band3-progress ";
constexpr std::string_view kSaved = "band3-file ";

std::string Path(const std::filesystem::path& p) {
    const std::u8string u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

// what every download asks for: one video, a line per progress step and the
// saved file's name in band3's words, the file's time left as now
std::vector<std::string> DownloadArgs(std::string_view url, const std::filesystem::path& out_stem) {
    return {"--no-playlist",
            "--no-mtime",
            "--force-overwrites",
            "--newline",
            "--progress",
            "--no-colors",
            "--progress-template",
            std::string("download:") + std::string(kProgress) +
                "%(progress.downloaded_bytes)s %(progress.total_bytes)s "
                "%(progress.total_bytes_estimate)s",
            "--print",
            std::string("after_move:") + std::string(kSaved) + "%(filepath)s",
            "-o",
            Path(out_stem) + ".%(ext)s",
            "--",
            std::string(url)};
}

int64_t Whole(std::string_view text) {
    // "NA", or a float ("1234.0") for an estimate
    double d = 0;
    const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), d);
    return ec == std::errc() && d > 0 ? int64_t(d) : 0;
}

}

std::optional<YtCandidate> ParseCandidate(std::string_view json_line) {
    const std::optional<json::Value> v = json::Parse(json_line);
    if (!v || !v->IsObject()) return std::nullopt;
    const json::Value& o = *v;
    YtCandidate c;
    c.id = o["id"].Text();
    if (c.id.empty()) return std::nullopt;
    // a channel or a playlist among search results isn't a video
    if (o["_type"].Text() == "url") {
        const std::string link = o["url"].Text();
        for (std::string_view not_video : {"/playlist?", "/channel/", "/@", "/c/", "/user/"})
            if (link.find(not_video) != std::string::npos) return std::nullopt;
    }
    c.url = o["webpage_url"].Text();
    if (c.url.empty()) c.url = o["original_url"].Text();
    if (c.url.empty()) c.url = o["url"].Text();
    c.title = o["title"].Text();
    c.channel = o["channel"].Text();
    if (c.channel.empty()) c.channel = o["uploader"].Text();
    c.description = o["description"].Text();
    c.duration = o["duration"].Number(0.0);
    c.views = o["view_count"].IsNumber() ? int64_t(o["view_count"].Number()) : -1;
    c.verified = o["channel_is_verified"].Bool(false);
    return c;
}

std::vector<std::string> SearchArgs(std::string_view query, int count) {
    return {"--flat-playlist", "--dump-json", "--no-warnings", "--no-colors",
            "ytsearch" + std::to_string(count) + ":" + std::string(query)};
}

std::vector<std::string> InfoArgs(std::string_view url) {
    return {"--dump-json", "--no-playlist", "--no-warnings", "--no-colors", "--", std::string(url)};
}

std::vector<std::string> AudioArgs(std::string_view url, const std::filesystem::path& out_stem) {
    std::vector<std::string> args = {"-f", "ba/b"};
    for (std::string& a : DownloadArgs(url, out_stem)) args.push_back(std::move(a));
    return args;
}

std::vector<std::string> VideoArgs(std::string_view url, const std::filesystem::path& out_stem,
                                   const VideoFormat& format) {
    // the picture alone where there's one apart, any whole file otherwise;
    // the tallest up to max_height, then the highest frame rate (or nearest
    // 30), then H.264 before the newer codecs
    const std::string sort = "res:" + std::to_string(format.max_height) +
                             (format.prefer_60fps ? ",fps" : ",fps:30") + ",+vcodec";
    std::vector<std::string> args = {"-f", "bv*/b", "-S", sort};
    for (std::string& a : DownloadArgs(url, out_stem)) args.push_back(std::move(a));
    return args;
}

std::vector<std::string> SplitArgs(std::string_view text) {
    std::vector<std::string> out;
    std::string cur;
    bool in_word = false, quoted = false;
    for (const char c : text) {
        if (c == '"') {
            quoted = !quoted;
            in_word = true;
        } else if ((c == ' ' || c == '\t') && !quoted) {
            if (in_word) out.push_back(std::move(cur));
            cur.clear();
            in_word = false;
        } else {
            cur += c;
            in_word = true;
        }
    }
    if (in_word) out.push_back(std::move(cur));
    return out;
}

std::optional<Progress> ParseProgress(std::string_view line) {
    if (!line.starts_with(kProgress)) return std::nullopt;
    line.remove_prefix(kProgress.size());
    std::string_view parts[3];
    for (auto& part : parts) {
        const size_t space = line.find(' ');
        part = line.substr(0, space);
        line.remove_prefix(space == std::string_view::npos ? line.size() : space + 1);
    }
    Progress p;
    p.done = Whole(parts[0]);
    p.total = Whole(parts[1]);
    if (p.total == 0) p.total = Whole(parts[2]);
    return p;
}

std::optional<std::string> ParseSavedFile(std::string_view line) {
    if (!line.starts_with(kSaved)) return std::nullopt;
    return std::string(line.substr(kSaved.size()));
}

std::optional<std::string> ParseError(std::string_view line) {
    constexpr std::string_view kError = "ERROR: ";
    if (!line.starts_with(kError)) return std::nullopt;
    line.remove_prefix(kError.size());
    // "[youtube] abc123: " before yt-dlp's words
    if (line.starts_with('[')) {
        const size_t colon = line.find(": ");
        if (colon != std::string_view::npos) line.remove_prefix(colon + 2);
    }
    return std::string(line);
}

YtDlp::YtDlp(std::filesystem::path exe, std::vector<std::string> extra_args)
    : exe_(exe.empty() ? std::filesystem::path("yt-dlp") : std::move(exe)),
      extra_(std::move(extra_args)) {
#ifdef _WIN32
    // yt-dlp is Python: without this, it writes in the console's code page,
    // which no console (CREATE_NO_WINDOW) leaves as the system's ANSI one
    SetEnvironmentVariableW(L"PYTHONIOENCODING", L"utf-8");
#endif
}

bool YtDlp::Run(std::vector<std::string> args, bool with_extra,
                const std::function<void(std::string_view)>& on_line, std::string& error,
                const std::atomic<bool>* cancel) const {
    if (with_extra) args.insert(args.begin(), extra_.begin(), extra_.end());
    std::string last_error, last_line;
    const ProcessResult r = RunProcess(
        exe_, args,
        [&](std::string_view line) {
            if (auto e = ParseError(line)) last_error = std::move(*e);
            if (!line.empty()) last_line = line;
            on_line(line);
        },
        cancel);
    if (!r.started) {
        error = "yt-dlp (" + Path(exe_) + ") " + r.error;
        return false;
    }
    if (r.cancelled) {
        error = "stopped";
        return false;
    }
    if (r.exit_code != 0) {
        error = !last_error.empty() ? last_error
                : !last_line.empty() ? last_line
                                     : "yt-dlp ended with code " + std::to_string(r.exit_code);
        return false;
    }
    return true;
}

std::optional<std::string> YtDlp::Version(std::string& error) const {
    std::string version;
    if (!Run({"--version"}, false,
             [&](std::string_view line) {
                 if (version.empty() && !line.empty()) version = line;
             },
             error, nullptr))
        return std::nullopt;
    if (version.empty()) {
        error = "yt-dlp didn't say its version";
        return std::nullopt;
    }
    return version;
}

std::vector<YtCandidate> YtDlp::Search(std::string_view query, int count, std::string& error,
                                       const std::atomic<bool>* cancel) const {
    std::vector<YtCandidate> out;
    Run(SearchArgs(query, count), true,
        [&](std::string_view line) {
            if (auto c = ParseCandidate(line)) out.push_back(std::move(*c));
        },
        error, cancel);
    return out;
}

std::optional<YtCandidate> YtDlp::Info(std::string_view url, std::string& error,
                                       const std::atomic<bool>* cancel) const {
    std::optional<YtCandidate> out;
    if (!Run(InfoArgs(url), true,
             [&](std::string_view line) {
                 if (!out) out = ParseCandidate(line);
             },
             error, cancel))
        return std::nullopt;
    if (!out) error = "yt-dlp found no video there";
    return out;
}

std::optional<std::filesystem::path> YtDlp::Download(
    const std::vector<std::string>& args, const std::function<void(const Progress&)>& progress,
    std::string& error, const std::atomic<bool>* cancel) const {
    std::optional<std::string> saved;
    if (!Run(args, true,
             [&](std::string_view line) {
                 if (auto p = ParseProgress(line)) {
                     if (progress) progress(*p);
                 } else if (auto f = ParseSavedFile(line)) {
                     saved = std::move(*f);
                 }
             },
             error, cancel))
        return std::nullopt;
    if (!saved) {
        error = "yt-dlp didn't say what it saved";
        return std::nullopt;
    }
    return std::filesystem::path(std::u8string(saved->begin(), saved->end()));
}

bool YtDlp::Update(std::string& output) const {
    output.clear();
    std::string error;
    const bool ok = Run({"-U"}, false,
                        [&](std::string_view line) {
                            if (line.empty()) return;
                            if (!output.empty()) output += '\n';
                            output += line;
                        },
                        error, nullptr);
    if (!ok && output.empty()) output = error;
    return ok;
}

}
