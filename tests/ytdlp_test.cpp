// Checks running another program (src/Video/child_process.cpp) and yt-dlp's
// arguments and output (src/Video/ytdlp.cpp), against tests/fake_ytdlp.cpp.

#include <doctest/doctest.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>
#include "src/Video/child_process.h"
#include "src/Video/ytdlp.h"

using namespace band3::video;
namespace fs = std::filesystem;

namespace {

const fs::path kFake = fs::path(BAND3_FAKE_YTDLP);

std::vector<std::string> Lines(const std::vector<std::string>& args) {
    std::vector<std::string> out;
    RunProcess(kFake, args, [&](std::string_view line) { out.emplace_back(line); });
    return out;
}

bool Has(const std::vector<std::string>& args, std::string_view a) {
    for (const auto& x : args)
        if (x == a) return true;
    return false;
}

}

TEST_CASE("a program's arguments arrive as they were, spaces, quotes and all") {
    const std::vector<std::string> args = {"plain",  "two words", "say \"hi\"", "ends\\",
                                           "a\\\\\"b", "",          "Caf\xC3\xA9 \xE2\x99\xAA"};
    std::vector<std::string> sent = {"--echo-args"};
    sent.insert(sent.end(), args.begin(), args.end());
    CHECK(Lines(sent) == args);
}

TEST_CASE("a program's exit code, and one that isn't there") {
    std::vector<std::string> lines;
    ProcessResult r = RunProcess(kFake, {"--dump-json", "--", "https://x/unavailable"},
                                 [&](std::string_view l) { lines.emplace_back(l); });
    CHECK(r.started);
    CHECK(r.exit_code == 1);
    REQUIRE(lines.size() == 1);
    CHECK(ParseError(lines[0]) == "Video unavailable");
    r = RunProcess(kFake.parent_path() / "no_such_program", {}, [](std::string_view) {});
    CHECK_FALSE(r.started);
    CHECK(r.error == "it isn't there");
}

TEST_CASE("a program is stopped when asked") {
    std::atomic<bool> cancel{false};
    const fs::path dir = fs::temp_directory_path() / "band3_ytdlp_cancel";
    fs::create_directories(dir);
    int lines = 0;
    const auto start = std::chrono::steady_clock::now();
    const ProcessResult r = RunProcess(
        kFake, {"-o", (dir / "x.%(ext)s").string(), "--", "https://x/slow"},
        [&](std::string_view) {
            if (++lines == 2) cancel = true;
        },
        &cancel);
    CHECK(r.cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(5));
    fs::remove_all(dir);
}

TEST_CASE("yt-dlp's search results and videos are read from its JSON") {
    YtDlp yt(kFake, {});
    std::string error;
    const auto results = yt.Search("Metallica Master of Puppets official music video", 8, error);
    CHECK(error.empty());
    // the channel among them isn't a video
    REQUIRE(results.size() == 5);
    CHECK(results[1].id == "offic0001");
    CHECK(results[1].title == "Metallica: Master of Puppets (Official Music Video)");
    CHECK(results[1].channel == "Metallica");
    CHECK(results[1].url == "https://www.youtube.com/watch?v=offic0001");
    CHECK(results[1].duration == 517.0);
    CHECK(results[1].views == 120000000);
    CHECK(results[1].verified);
    CHECK(results[0].description.empty());
    CHECK(results[2].description.starts_with("Provided to YouTube by"));
    CHECK(results[2].channel == "Metallica - Topic");
    const auto info = yt.Info("https://vimeo.com/123", error);
    REQUIRE(info);
    CHECK(info->url == "https://vimeo.com/123");
    CHECK(info->duration == 15.0);
    CHECK_FALSE(yt.Info("https://x/unavailable", error));
    CHECK(error == "Video unavailable");
    CHECK(yt.Version(error) == "2026.09.30");
    std::string said;
    CHECK(yt.Update(said));
    CHECK(said.find("up to date") != std::string::npos);
    // not there
    YtDlp missing(kFake.parent_path() / "no_such_ytdlp", {});
    CHECK_FALSE(missing.Version(error));
    CHECK(error.find("isn't there") != std::string::npos);
}

TEST_CASE("yt-dlp downloads say how far they are and what they saved") {
    YtDlp yt(kFake, {"--cookies-from-browser", "firefox"});
    const fs::path dir = fs::temp_directory_path() / "band3_ytdlp_download";
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::vector<Progress> steps;
    std::string error;
    const auto saved = yt.Download(AudioArgs("https://www.youtube.com/watch?v=clicks", dir / "song.audio"),
                                   [&](const Progress& p) { steps.push_back(p); }, error);
    REQUIRE_MESSAGE(saved, error);
    CHECK(*saved == dir / "song.audio.webm");
    CHECK(fs::exists(*saved));
    REQUIRE(steps.size() == 3);
    CHECK(steps.back().done == steps.back().total);
    const auto video = yt.Download(VideoArgs("https://www.youtube.com/watch?v=clicks", dir / "song.video", {}),
                                   nullptr, error);
    REQUIRE(video);
    CHECK(video->extension() == ".mp4");
    CHECK_FALSE(yt.Download(AudioArgs("https://x/fail", dir / "f"), nullptr, error));
    CHECK(error == "Video unavailable");
    fs::remove_all(dir);
}

TEST_CASE("yt-dlp's arguments ask for one video's sound, or its picture up to a height") {
    const auto audio = AudioArgs("https://youtu.be/abc", fs::path("v") / "song");
    CHECK(Has(audio, "ba/b"));
    CHECK(Has(audio, "--no-playlist"));
    CHECK(audio.back() == "https://youtu.be/abc");
    CHECK(audio[audio.size() - 2] == "--");
    auto video = VideoArgs("u", "song", {720, true});
    CHECK(Has(video, "bv*/b"));
    CHECK(Has(video, "res:720,fps,+vcodec"));
    video = VideoArgs("u", "song", {1080, false});
    CHECK(Has(video, "res:1080,fps:30,+vcodec"));
    const auto search = SearchArgs("a b", 8);
    CHECK(search.back() == "ytsearch8:a b");
    CHECK(Has(search, "--flat-playlist"));
}

TEST_CASE("the player's extra yt-dlp flags are split at spaces, quoted parts kept") {
    CHECK(SplitArgs("") == std::vector<std::string>{});
    CHECK(SplitArgs("  --cookies-from-browser firefox ") ==
          std::vector<std::string>{"--cookies-from-browser", "firefox"});
    CHECK(SplitArgs(R"(--cookies "C:\My Files\c.txt" -4)") ==
          std::vector<std::string>{"--cookies", R"(C:\My Files\c.txt)", "-4"});
}

TEST_CASE("yt-dlp's progress, saved file and error lines") {
    auto p = ParseProgress("band3-progress 1024 4096 NA");
    REQUIRE(p);
    CHECK(p->done == 1024);
    CHECK(p->total == 4096);
    p = ParseProgress("band3-progress 10 NA 2000.5");
    REQUIRE(p);
    CHECK(p->total == 2000);
    CHECK_FALSE(ParseProgress("[download] 10%"));
    CHECK(ParseSavedFile("band3-file C:\\v\\a b.webm") == "C:\\v\\a b.webm");
    CHECK(ParseError("ERROR: Sign in to confirm your age") == "Sign in to confirm your age");
    CHECK_FALSE(ParseError("WARNING: x"));
}

TEST_CASE("yt-dlp's errors are cut to their first sentence, with what to do for sign-ins") {
    CHECK(Briefly("Video unavailable") == "Video unavailable");
    CHECK(Briefly("unable to download video data: HTTP Error 403: Forbidden") ==
          "unable to download video data: HTTP Error 403: Forbidden");
    const std::string age = Briefly(
        "Sign in to confirm your age. This video may be inappropriate. Use --cookies-from-browser");
    CHECK(age.starts_with("Sign in to confirm your age (yt-dlp needs your browser's cookies"));
    std::string error;
    YtDlp yt(kFake, {});
    CHECK_FALSE(yt.Info("https://x/agegate", error));
    CHECK(error.starts_with("Sign in to confirm your age ("));
}

TEST_CASE("yt-dlp is told to use any JavaScript runtime, where it takes the option") {
    YtDlp yt(kFake, {});
    CHECK(yt.Supports("--js-runtimes"));
    CHECK_FALSE(yt.Supports("--no-such-option"));
    const auto args = JsRuntimeArgs();
    CHECK(args.size() == 8);
    CHECK(Has(args, "node"));
    CHECK(Has(args, "deno"));
    // a runtime on PATH: the fake stands in for one, named node
    const fs::path dir = fs::temp_directory_path() / "band3_js_runtime";
    fs::create_directories(dir);
#ifdef _WIN32
    std::ofstream(dir / "node.exe") << "x";
    const std::string sep = ";";
#else
    std::ofstream(dir / "node") << "x";
    const std::string sep = ":";
#endif
    CHECK(JsRuntimeOnPath("C:\\nowhere" + sep + dir.string()));
    CHECK_FALSE(JsRuntimeOnPath("C:\\nowhere" + sep));
    CHECK_FALSE(JsRuntimeOnPath(""));
    fs::remove_all(dir);
}

TEST_CASE("Windows command lines quote what needs it") {
    CHECK(QuoteWindowsArgument("abc") == "abc");
    CHECK(QuoteWindowsArgument("") == "\"\"");
    CHECK(QuoteWindowsArgument("a b") == "\"a b\"");
    CHECK(QuoteWindowsArgument("a\"b") == "\"a\\\"b\"");
    CHECK(QuoteWindowsArgument("a b\\") == "\"a b\\\\\"");
    CHECK(QuoteWindowsArgument("C:\\x\\y") == "C:\\x\\y");
}
