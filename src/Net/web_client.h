#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

// band3 asking other sites for things over HTTPS (for RhythmVerse, through
// src/Net/song_downloads.h): Windows' WinHTTP, so nothing more to ship. On
// other systems every request fails, saying so.

namespace band3::web {

struct Reply {
    int status = 0;     // 0 when there was no reply
    std::string body;
    std::string error;  // why there was no reply
};

// a POST of a form (application/x-www-form-urlencoded); replies past max_bytes
// are cut off, as an error
Reply PostForm(std::string_view url, std::string_view form, size_t max_bytes = 16 * 1024 * 1024);

// What a download has read: the first bytes of the reply (`peek` of them, or
// all of a shorter file) for `check` to say what's wrong with them ("" for
// nothing, so it goes on), and then how far along it is (false stops it).
struct DownloadHooks {
    size_t peek = 0;
    std::function<std::string(std::string_view first_bytes)> check;
    std::function<bool(int64_t received, int64_t total)> progress;  // total 0 when unknown
};

// a GET of url saved to path (made anew); "" when it's all there, or why not.
// A failed or stopped download leaves no file.
std::string Download(std::string_view url, const std::filesystem::path& path,
                     const DownloadHooks& hooks, int64_t max_bytes);

}
