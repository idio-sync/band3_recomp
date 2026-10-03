#include "web_client.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#elif defined(BAND3_HAVE_CURL)
#include <curl/curl.h>
#include <memory>
#include <mutex>
#include <optional>
#endif

#include <charconv>
#include <fstream>
#include <vector>
#include <fmt/format.h>

namespace band3::web {

namespace {

// A download on its way to its file, a piece at a time, then an empty piece at
// its end: its first bytes go to the hooks' check before any is written, and
// each piece to their progress. Take is false once it has stopped, error
// saying why.
class Saver {
public:
    Saver(const std::filesystem::path& path, const DownloadHooks& hooks, int64_t max_bytes,
          int64_t total)
        : path_(path),
          hooks_(hooks),
          max_bytes_(max_bytes),
          total_(total),
          file_(path, std::ios::binary | std::ios::trunc),
          made_(static_cast<bool>(file_)),
          checked_(!hooks.check || hooks.peek == 0) {
        if (!made_) error = "couldn't make the file";
    }

    bool Take(std::string_view piece) {
        if (!error.empty()) return false;
        received_ += static_cast<int64_t>(piece.size());
        if (received_ > max_bytes_) return Stop("the file is too big");
        if (!checked_) {
            first_.append(piece);
            if (first_.size() < hooks_.peek && !piece.empty()) return true;
            checked_ = true;
            if (std::string why = hooks_.check(first_); !why.empty()) return Stop(std::move(why));
            file_.write(first_.data(), static_cast<std::streamsize>(first_.size()));
        } else {
            file_.write(piece.data(), static_cast<std::streamsize>(piece.size()));
        }
        if (!file_) return Stop("couldn't write the file (is the disk full?)");
        if (hooks_.progress && !hooks_.progress(received_, total_)) return Stop("stopped");
        return true;
    }

    // closes the file, which a download that stopped short (with failure, if
    // it hadn't stopped already) doesn't leave behind: "" or why
    std::string Finish(std::string failure = {}) {
        if (error.empty()) error = std::move(failure);
        file_.close();
        if (error.empty() && total_ && received_ != total_) error = "the download was cut short";
        if (!error.empty() && made_) {
            std::error_code ec;
            std::filesystem::remove(path_, ec);
        }
        return error;
    }

    std::string error;

private:
    bool Stop(std::string why) {
        error = std::move(why);
        return false;
    }

    std::filesystem::path path_;
    const DownloadHooks& hooks_;
    int64_t max_bytes_;
    int64_t total_;  // 0 when not known
    std::ofstream file_;
    bool made_;
    bool checked_;
    std::string first_;  // the bytes `check` sees, until it has
    int64_t received_ = 0;
};

}  // namespace

#ifdef _WIN32

namespace {

constexpr wchar_t kAgent[] = L"band3 (Rock Band 3 recompilation)";
// resolving and connecting, then each send and each wait for data (ms)
constexpr int kConnectTimeout = 15000;
constexpr int kDataTimeout = 30000;

class Handle {
public:
    Handle() = default;
    explicit Handle(HINTERNET h) : h_(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle& operator=(Handle&& other) noexcept {
        std::swap(h_, other.h_);
        return *this;
    }
    ~Handle() {
        if (h_) WinHttpCloseHandle(h_);
    }
    HINTERNET get() const { return h_; }
    explicit operator bool() const { return h_ != nullptr; }

private:
    HINTERNET h_ = nullptr;
};

std::wstring Wide(std::string_view text) {
    if (text.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                      nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

// the last WinHTTP error, as words where there are some
std::string Failure(const char* doing) {
    const DWORD error = GetLastError();
    switch (error) {
        case ERROR_WINHTTP_NAME_NOT_RESOLVED:
            return "couldn't find the site (is this PC online?)";
        case ERROR_WINHTTP_CANNOT_CONNECT:
            return "couldn't connect to the site";
        case ERROR_WINHTTP_TIMEOUT:
            return "the site took too long to answer";
        case ERROR_WINHTTP_CONNECTION_ERROR:
            return "the connection broke";
        case ERROR_WINHTTP_SECURE_FAILURE:
            return "the site's certificate wasn't accepted";
        default:
            return fmt::format("{} failed (WinHTTP error {})", doing, error);
    }
}

struct Exchange {
    Handle session;
    Handle connection;
    Handle request;
    int status = 0;
    int64_t length = 0;  // Content-Length; 0 when not sent
};

// sends the request and reads the reply's head; "" or why it couldn't
std::string Send(Exchange& x, std::string_view url, const wchar_t* verb,
                 std::wstring_view headers, std::string_view body, bool decompress) {
    const std::wstring wide_url = Wide(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), 0, 0, &parts)) return "not a URL: " + std::string(url);
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring path(parts.lpszUrlPath, parts.dwUrlPathLength);
    path.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);

    // the system's proxy settings; WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY needs Windows 8.1
    x.session = Handle(WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!x.session) {
        x.session = Handle(WinHttpOpen(kAgent, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                       WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    }
    if (!x.session) return Failure("WinHttpOpen");
    WinHttpSetTimeouts(x.session.get(), kConnectTimeout, kConnectTimeout, kDataTimeout,
                       kDataTimeout);
    x.connection = Handle(WinHttpConnect(x.session.get(), host.c_str(), parts.nPort, 0));
    if (!x.connection) return Failure("WinHttpConnect");
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    x.request = Handle(WinHttpOpenRequest(x.connection.get(), verb, path.c_str(), nullptr,
                                          WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!x.request) return Failure("WinHttpOpenRequest");
    if (decompress) {
        // gzip and deflate replies, where Windows can (8.1 on); without, they come plain
        DWORD option = WINHTTP_DECOMPRESSION_FLAG_ALL;
        WinHttpSetOption(x.request.get(), WINHTTP_OPTION_DECOMPRESSION, &option, sizeof(option));
    }
    const std::wstring header_text(headers);
    if (!WinHttpSendRequest(x.request.get(),
                            header_text.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : header_text.c_str(),
                            header_text.empty() ? 0 : static_cast<DWORD>(-1),
                            const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
                            static_cast<DWORD>(body.size()), 0) ||
        !WinHttpReceiveResponse(x.request.get(), nullptr)) {
        return Failure("sending the request");
    }

    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(x.request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX)) {
        return Failure("reading the reply");
    }
    x.status = static_cast<int>(status);

    wchar_t length[32];
    size = sizeof(length);
    if (WinHttpQueryHeaders(x.request.get(), WINHTTP_QUERY_CONTENT_LENGTH,
                            WINHTTP_HEADER_NAME_BY_INDEX, length, &size, WINHTTP_NO_HEADER_INDEX)) {
        std::string digits;
        for (const wchar_t* c = length; *c; c++) digits += static_cast<char>(*c);
        std::from_chars(digits.data(), digits.data() + digits.size(), x.length);
    }
    return {};
}

// the next piece of the reply into buf; 0 at its end, -1 on an error
int64_t Read(Exchange& x, std::vector<char>& buf) {
    DWORD read = 0;
    if (!WinHttpReadData(x.request.get(), buf.data(), static_cast<DWORD>(buf.size()), &read)) {
        return -1;
    }
    return read;
}

}

Reply PostForm(std::string_view url, std::string_view form, size_t max_bytes) {
    Reply reply;
    Exchange x;
    reply.error = Send(x, url, L"POST",
                       L"Content-Type: application/x-www-form-urlencoded\r\n"
                       L"Accept: application/json\r\n",
                       form, true);
    if (!reply.error.empty()) return reply;
    reply.status = x.status;
    std::vector<char> buf(64 * 1024);
    while (true) {
        const int64_t n = Read(x, buf);
        if (n < 0) {
            reply.error = Failure("reading the reply");
            return reply;
        }
        if (n == 0) break;
        reply.body.append(buf.data(), static_cast<size_t>(n));
        if (reply.body.size() > max_bytes) {
            reply.error = "the reply was too big";
            return reply;
        }
    }
    return reply;
}

std::string Download(std::string_view url, const std::filesystem::path& path,
                     const DownloadHooks& hooks, int64_t max_bytes) {
    Exchange x;
    if (std::string error = Send(x, url, L"GET", {}, {}, false); !error.empty()) return error;
    if (x.status != 200) return fmt::format("the site answered {}", x.status);
    if (x.length > max_bytes) return "the file is too big";

    Saver saver(path, hooks, max_bytes, x.length);
    if (!saver.error.empty()) return saver.Finish();
    std::vector<char> buf(256 * 1024);
    while (true) {
        const int64_t n = Read(x, buf);
        if (n < 0) return saver.Finish(Failure("downloading"));
        if (!saver.Take(std::string_view(buf.data(), static_cast<size_t>(n))) || n == 0) break;
    }
    return saver.Finish();
}

#elif defined(BAND3_HAVE_CURL)

namespace {

constexpr char kAgent[] = "band3 (Rock Band 3 recompilation)";
// resolving and connecting (ms), and how long a reply may stall (s)
constexpr long kConnectTimeout = 15000;
constexpr long kDataTimeout = 30;

using Easy = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using Headers = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

// a request for url, set up as WinHTTP's are: the proxy from the environment
// (https_proxy and the like), redirects followed, the system's certificates
Easy Open(std::string_view url) {
    static std::once_flag once;
    std::call_once(once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });
    Easy curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) return curl;
    CURL* c = curl.get();
    curl_easy_setopt(c, CURLOPT_URL, std::string(url).c_str());  // copied
    curl_easy_setopt(c, CURLOPT_USERAGENT, kAgent);
    // requests run on band3's threads, where curl's timeouts can't use signals
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_MAXREDIRS, 10L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT_MS, kConnectTimeout);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, kDataTimeout);
    return curl;
}

// a curl error, as words where there are some
std::string Failure(CURLcode code, const char* doing) {
    switch (code) {
        case CURLE_URL_MALFORMAT:
            return "not a URL";
        case CURLE_COULDNT_RESOLVE_HOST:
        case CURLE_COULDNT_RESOLVE_PROXY:
            return "couldn't find the site (is this PC online?)";
        case CURLE_COULDNT_CONNECT:
            return "couldn't connect to the site";
        case CURLE_OPERATION_TIMEDOUT:
            return "the site took too long to answer";
        case CURLE_SEND_ERROR:
        case CURLE_RECV_ERROR:
        case CURLE_GOT_NOTHING:
        case CURLE_PARTIAL_FILE:
            return "the connection broke";
        case CURLE_PEER_FAILED_VERIFICATION:
            return "the site's certificate wasn't accepted";
        default:
            return fmt::format("{} failed (curl error {}: {})", doing, static_cast<int>(code),
                               curl_easy_strerror(code));
    }
}

int Status(CURL* curl) {
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    return static_cast<int>(status);
}

struct Collect {
    std::string body;
    size_t max_bytes;
    bool too_big = false;
};

size_t CollectPiece(char* data, size_t size, size_t count, void* user) {
    auto& c = *static_cast<Collect*>(user);
    const size_t n = size * count;
    if (c.body.size() + n > c.max_bytes) {
        c.too_big = true;
        return 0;  // stops it
    }
    c.body.append(data, n);
    return n;
}

struct Fetch {
    CURL* curl;
    const std::filesystem::path& path;
    const DownloadHooks& hooks;
    int64_t max_bytes;
    std::optional<Saver> saver;  // once the reply's head says to save it
    std::string error;           // why it didn't
};

// by the reply's first piece its head is in: whether to save it
bool Begin(Fetch& f) {
    if (const int status = Status(f.curl); status != 200) {
        f.error = fmt::format("the site answered {}", status);
        return false;
    }
    curl_off_t length = -1;
    curl_easy_getinfo(f.curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &length);
    const int64_t total = length > 0 ? static_cast<int64_t>(length) : 0;
    if (total > f.max_bytes) {
        f.error = "the file is too big";
        return false;
    }
    f.saver.emplace(f.path, f.hooks, f.max_bytes, total);
    return f.saver->error.empty();
}

size_t SavePiece(char* data, size_t size, size_t count, void* user) {
    auto& f = *static_cast<Fetch*>(user);
    if (!f.saver && !Begin(f)) return 0;  // stops it
    const size_t n = size * count;
    return f.saver->Take(std::string_view(data, n)) ? n : 0;
}

}  // namespace

Reply PostForm(std::string_view url, std::string_view form, size_t max_bytes) {
    Reply reply;
    Easy curl = Open(url);
    if (!curl) {
        reply.error = "couldn't start a request";
        return reply;
    }
    CURL* c = curl.get();
    curl_slist* list = curl_slist_append(nullptr, "Content-Type: application/x-www-form-urlencoded");
    list = curl_slist_append(list, "Accept: application/json");
    const Headers headers(list, &curl_slist_free_all);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(c, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(form.size()));
    curl_easy_setopt(c, CURLOPT_POSTFIELDS, form.data());  // not copied: form outlives the request
    // every encoding this libcurl can undo
    curl_easy_setopt(c, CURLOPT_ACCEPT_ENCODING, "");
    Collect collect{{}, max_bytes};
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, &CollectPiece);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, &collect);

    const CURLcode code = curl_easy_perform(c);
    reply.status = Status(c);
    if (collect.too_big) {
        reply.error = "the reply was too big";
    } else if (code != CURLE_OK) {
        reply.error = Failure(code, "the request");
    } else {
        reply.body = std::move(collect.body);
    }
    return reply;
}

std::string Download(std::string_view url, const std::filesystem::path& path,
                     const DownloadHooks& hooks, int64_t max_bytes) {
    Easy curl = Open(url);
    if (!curl) return "couldn't start a request";
    Fetch f{curl.get(), path, hooks, max_bytes};
    curl_easy_setopt(f.curl, CURLOPT_WRITEFUNCTION, &SavePiece);
    curl_easy_setopt(f.curl, CURLOPT_WRITEDATA, &f);

    const CURLcode code = curl_easy_perform(f.curl);
    // an empty reply has no pieces, so its head is looked at now
    if (code == CURLE_OK && !f.saver) Begin(f);
    if (!f.saver) return f.error.empty() ? Failure(code, "downloading") : f.error;
    if (code != CURLE_OK) return f.saver->Finish(Failure(code, "downloading"));
    f.saver->Take({});  // its end
    return f.saver->Finish();
}

#else

Reply PostForm(std::string_view, std::string_view, size_t) {
    return Reply{0, {}, "band3 was built without libcurl, so it can't reach other sites"};
}

std::string Download(std::string_view, const std::filesystem::path&, const DownloadHooks&,
                     int64_t) {
    return "band3 was built without libcurl, so it can't download";
}

#endif

}
