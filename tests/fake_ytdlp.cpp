// A stand-in for yt-dlp, for the tests of running it (src/Video/ytdlp.h,
// child_process.h, video_fetch.h): it answers as yt-dlp does, from
// tests/data, without the network.
//   --version, -U              a version, an update's message
//   --echo-args ...            each argument back on a line of its own
//   ytsearchN:<query>          tests/data/ytdlp_search.jsonl's lines
//   --dump-json <url>          one video's description; "unavailable" in it fails
//   -o <stem>.%(ext)s <url>    a download, copied from tests/data by the link:
//                              "clicks_late" or "offic" sync_clicks_late.m4a, else
//                              sync_clicks.webm; a picture (-f bv*/b or b) the
//                              red-blue clip (with FAKE_YTDLP_STILL set, a still
//                              one); "slow" takes 10 s, "fail" fails,
//                              "agegate" asks to sign in, "flaky" refuses -f bv*/b

#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace {

fs::path Data(const char* name) { return fs::path(BAND3_ROOT_DIR) / "tests" / "data" / name; }

bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int Run(const std::vector<std::string>& args) {
    if (args.empty()) return 2;
    if (args[0] == "--echo-args") {
        for (size_t i = 1; i < args.size(); i++) std::cout << args[i] << "\n";
        return 0;
    }
    std::string url, out, format;
    bool dump = false;
    for (size_t i = 0; i < args.size(); i++) {
        const std::string& a = args[i];
        if (a == "--version") {
            std::cout << "2026.09.30\n";
            return 0;
        }
        if (a == "--help") {
            std::cout << "Usage: yt-dlp [OPTIONS] URL [URL...]\n"
                      << "    --js-runtimes RUNTIME[:PATH]    Additional JavaScript runtime to enable\n";
            return 0;
        }
        if (a == "-U") {
            std::cout << "Latest version: stable@2026.09.30 from yt-dlp/yt-dlp\n"
                      << "yt-dlp is up to date (stable@2026.09.30 from yt-dlp/yt-dlp)\n";
            return 0;
        }
        if (a == "--dump-json") dump = true;
        if (a == "-o" && i + 1 < args.size()) out = args[++i];
        else if (a == "-f" && i + 1 < args.size()) format = args[++i];
        else if (a == "--") {
            if (i + 1 < args.size()) url = args[++i];
        } else if (a.rfind("ytsearch", 0) == 0) {
            std::ifstream in(Data("ytdlp_search.jsonl"));
            for (std::string line; std::getline(in, line);) std::cout << line << "\n";
            return 0;
        }
    }
    if (Has(url, "unavailable") || Has(url, "fail")) {
        std::cerr << "ERROR: [youtube] abc123: Video unavailable\n";
        return 1;
    }
    if (Has(url, "agegate")) {
        std::cerr << "ERROR: [youtube] abc123: Sign in to confirm your age. This video may be "
                     "inappropriate for some users. Use --cookies-from-browser or --cookies\n";
        return 1;
    }
    // the picture alone refused, as YouTube now and then does
    if (Has(url, "flaky") && format == "bv*/b") {
        std::cerr << "ERROR: unable to download video data: HTTP Error 403: Forbidden\n";
        return 1;
    }
    if (dump) {
        std::cout << R"({"id": "pasted1", "title": "A pasted video", "webpage_url": ")" << url
                  << R"(", "channel": "Someone", "duration": 15.0})" << "\n";
        return 0;
    }
    if (out.empty() || url.empty()) return 2;
    // FAKE_YTDLP_STILL set: every picture's a still one
    const bool still = std::getenv("FAKE_YTDLP_STILL") != nullptr;
    const fs::path from = format == "bv*/b" || format == "b"
                              ? Data(still ? "still_picture.mp4" : "music_video_red_blue.mp4")
                          : Has(url, "clicks_late") || Has(url, "offic") ? Data("sync_clicks_late.m4a")
                                                                         : Data("sync_clicks.webm");
    const size_t at = out.find("%(ext)s");
    if (at == std::string::npos) return 2;
    std::string to = out;
    to.replace(at, 7, from.extension().string().substr(1));
    const auto size = fs::file_size(from);
    const int steps = Has(url, "slow") ? 100 : 3;
    for (int i = 1; i <= steps; i++) {
        std::cout << "band3-progress " << size * i / steps << " " << size << " NA" << std::endl;
        if (Has(url, "slow")) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::error_code ec;
    fs::copy_file(from, fs::path(std::u8string(to.begin(), to.end())),
                  fs::copy_options::overwrite_existing, ec);
    if (ec) {
        std::cerr << "ERROR: unable to write " << to << "\n";
        return 1;
    }
    std::cout << "band3-file " << to << "\n";
    return 0;
}

}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
    // UTF-8 out, as yt-dlp with PYTHONIOENCODING=utf-8
    std::vector<std::string> args;
    for (int i = 1; i < argc; i++) {
        const int n = WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, nullptr, 0, nullptr, nullptr);
        std::string s(size_t(n > 0 ? n - 1 : 0), '\0');
        WideCharToMultiByte(CP_UTF8, 0, argv[i], -1, s.data(), n, nullptr, nullptr);
        args.push_back(std::move(s));
    }
    return Run(args);
}
#else
int main(int argc, char** argv) { return Run(std::vector<std::string>(argv + 1, argv + argc)); }
#endif
