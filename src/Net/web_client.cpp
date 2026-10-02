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
#endif

#include <charconv>
#include <fstream>
#include <vector>
#include <fmt/format.h>

namespace band3::web {

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

    std::string error;
    {
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        if (!file) return "couldn't make the file";
        std::vector<char> buf(256 * 1024);
        std::string first;  // the bytes `check` sees, until it has
        bool checked = !hooks.check || hooks.peek == 0;
        int64_t received = 0;
        while (error.empty()) {
            const int64_t n = Read(x, buf);
            if (n < 0) {
                error = Failure("downloading");
                break;
            }
            const std::string_view piece(buf.data(), static_cast<size_t>(n));
            received += n;
            if (received > max_bytes) {
                error = "the file is too big";
                break;
            }
            if (!checked) {
                first.append(piece);
                if (first.size() < hooks.peek && n > 0) continue;
                checked = true;
                error = hooks.check(first);
                if (!error.empty()) break;
                file.write(first.data(), static_cast<std::streamsize>(first.size()));
            } else {
                file.write(piece.data(), static_cast<std::streamsize>(piece.size()));
            }
            if (!file) {
                error = "couldn't write the file (is the disk full?)";
                break;
            }
            if (hooks.progress && !hooks.progress(received, x.length)) {
                error = "stopped";
                break;
            }
            if (n == 0) break;
        }
        if (error.empty() && x.length && received != x.length) error = "the download was cut short";
    }
    if (!error.empty()) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
    }
    return error;
}

#else

Reply PostForm(std::string_view, std::string_view, size_t) {
    return Reply{0, {}, "band3 can only reach other sites on Windows for now"};
}

std::string Download(std::string_view, const std::filesystem::path&, const DownloadHooks&,
                     int64_t) {
    return "band3 can only download on Windows for now";
}

#endif

}
