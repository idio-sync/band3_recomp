#include "discord.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <iterator>
#include <mutex>
#include <thread>
#include <rex/logging.h>
#include "src/settings.h"

namespace band3::discord {

namespace detail {

namespace {

// Discord rejects details/state shorter than 2 characters
constexpr size_t kMinChars = 2;
constexpr size_t kMaxChars = 128;

bool IsContinuation(unsigned char c) { return (c & 0xC0) == 0x80; }

bool IsValidUtf8(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = s[i];
        size_t extra;
        if (c < 0x80) extra = 0;
        else if (c >= 0xC2 && c <= 0xDF) extra = 1;
        else if (c >= 0xE0 && c <= 0xEF) extra = 2;
        else if (c >= 0xF0 && c <= 0xF4) extra = 3;
        else return false;
        for (size_t k = 1; k <= extra; k++) {
            if (i + k >= s.size() || !IsContinuation(s[i + k])) return false;
        }
        i += extra + 1;
    }
    return true;
}

size_t CountChars(std::string_view utf8) {
    return std::count_if(utf8.begin(), utf8.end(),
                         [](char c) { return !IsContinuation(static_cast<unsigned char>(c)); });
}

void AppendJsonString(std::string& out, std::string_view s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

// fits text to Discord's 2..128 character limits
std::string Field(std::string text) {
    text = Truncate(text, kMaxChars);
    while (CountChars(text) < kMinChars) text += "\xE2\x80\x8B";  // zero-width space
    return text;
}

}

std::string ToUtf8(std::string_view text) {
    if (IsValidUtf8(text)) return std::string(text);
    std::string out;
    out.reserve(text.size() * 2);
    for (unsigned char c : text) {
        if (c < 0x80) {
            out += static_cast<char>(c);
        } else {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

std::string Truncate(std::string_view utf8, size_t max_chars) {
    size_t chars = 0;
    for (size_t i = 0; i < utf8.size(); i++) {
        if (!IsContinuation(static_cast<unsigned char>(utf8[i]))) {
            if (chars == max_chars) return std::string(utf8.substr(0, i));
            chars++;
        }
    }
    return std::string(utf8);
}

std::string BandText(const events::BandInfo& band) {
    // RB3E's TrackType and Difficulty enums
    static const char* const kTracks[] = {"Drums", "Guitar", "Bass", "Vocals", "Keys",
                                          "Pro Keys", "Pro Guitar", "Pro Guitar",
                                          "Pro Bass", "Pro Bass"};
    static const char* const kDifficulties[] = {"Easy", "Medium", "Hard", "Expert"};

    std::string text;
    for (int i = 0; i < 4; i++) {
        if (!band.member_exists[i] || band.track_type[i] >= std::size(kTracks)) continue;
        if (!text.empty()) text += " \xC2\xB7 ";  // middle dot
        text += kTracks[band.track_type[i]];
        if (band.difficulty[i] < std::size(kDifficulties)) {
            text += " (";
            text += kDifficulties[band.difficulty[i]];
            text += ")";
        }
    }
    return Truncate(text, kMaxChars);
}

std::string ActivityJson(const Presence& p, uint32_t pid, uint64_t nonce, bool clear) {
    std::string json = "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":" + std::to_string(pid);
    if (!clear) {
        std::string details, state, large_text = "Rock Band 3 Deluxe";
        if (p.playing) {
            details = p.title.empty() ? "Unknown Song" : p.title;
            state = p.artist.empty() ? "Playing" : "by " + p.artist;
            if (!p.band.empty()) large_text = p.band;
        } else {
            details = "Browsing Songs";
            state = "In Menus";
        }

        json += ",\"activity\":{\"details\":";
        AppendJsonString(json, Field(details));
        json += ",\"state\":";
        AppendJsonString(json, Field(state));
        if (p.playing && p.start > 0) {
            json += ",\"timestamps\":{\"start\":" + std::to_string(p.start) + "}";
        }
        json += ",\"assets\":{\"large_image\":\"guitar\",\"large_text\":";
        AppendJsonString(json, Field(large_text));
        json += "}}";
    }
    json += "},\"nonce\":\"" + std::to_string(nonce) + "\"}";
    return json;
}

std::string Frame(uint32_t opcode, std::string_view json) {
    std::string frame(8, '\0');
    uint32_t length = static_cast<uint32_t>(json.size());
    for (int i = 0; i < 4; i++) {
        frame[i] = static_cast<char>((opcode >> (8 * i)) & 0xFF);
        frame[4 + i] = static_cast<char>((length >> (8 * i)) & 0xFF);
    }
    frame.append(json);
    return frame;
}

}

namespace {

using Clock = std::chrono::steady_clock;

// RB3 Deluxe's Discord application: "Rock Band 3 Deluxe" with the "guitar" asset
constexpr char kClientId[] = "1125571051607298190";
constexpr auto kRetryInterval = std::chrono::seconds(15);
constexpr auto kSendInterval = std::chrono::seconds(4);  // Discord allows ~5 updates / 20 s
constexpr int kHandshakeTimeoutMs = 3000;

constexpr uint32_t kOpHandshake = 0;
constexpr uint32_t kOpFrame = 1;

// one connection to the Discord client's IPC endpoint
class Connection {
public:
    ~Connection() { Close(); }

#ifdef _WIN32
    bool Open() {
        for (int i = 0; i < 10; i++) {
            std::wstring name = L"\\\\?\\pipe\\discord-ipc-" + std::to_wstring(i);
            HANDLE pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                pipe_ = pipe;
                return true;
            }
        }
        return false;
    }

    void Close() {
        if (pipe_ != INVALID_HANDLE_VALUE) CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }

    bool IsOpen() const { return pipe_ != INVALID_HANDLE_VALUE; }

    bool Write(const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
            DWORD written = 0;
            if (!WriteFile(pipe_, data.data() + done, static_cast<DWORD>(data.size() - done),
                           &written, nullptr)) {
                return false;
            }
            done += written;
        }
        return true;
    }

private:
    // bytes waiting to be read, or -1 if the pipe is broken
    long Available() {
        DWORD avail = 0;
        if (!PeekNamedPipe(pipe_, nullptr, 0, nullptr, &avail, nullptr)) return -1;
        return static_cast<long>(avail);
    }

    long ReadSome(char* buf, size_t len) {
        DWORD got = 0;
        if (!ReadFile(pipe_, buf, static_cast<DWORD>(len), &got, nullptr)) return -1;
        return static_cast<long>(got);
    }

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
#else
    bool Open() {
        const char* dirs[] = {std::getenv("XDG_RUNTIME_DIR"), std::getenv("TMPDIR"),
                              std::getenv("TMP"), std::getenv("TEMP"), "/tmp"};
        const char* subdirs[] = {"", "app/com.discordapp.Discord/", "snap.discord/"};
        for (const char* dir : dirs) {
            if (!dir || !*dir) continue;
            for (const char* sub : subdirs) {
                for (int i = 0; i < 10; i++) {
                    std::string path = std::string(dir) + "/" + sub + "discord-ipc-" + std::to_string(i);
                    sockaddr_un addr{};
                    if (path.size() >= sizeof(addr.sun_path)) continue;
                    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
                    if (fd < 0) return false;
                    addr.sun_family = AF_UNIX;
                    std::memcpy(addr.sun_path, path.c_str(), path.size() + 1);
                    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
                        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
                        fd_ = fd;
                        return true;
                    }
                    close(fd);
                }
            }
        }
        return false;
    }

    void Close() {
        if (fd_ >= 0) close(fd_);
        fd_ = -1;
    }

    bool IsOpen() const { return fd_ >= 0; }

    bool Write(const std::string& data) {
        size_t done = 0;
        while (done < data.size()) {
            ssize_t n = send(fd_, data.data() + done, data.size() - done, MSG_NOSIGNAL);
            if (n < 0) {
                if (errno != EAGAIN && errno != EWOULDBLOCK) return false;
                pollfd p{fd_, POLLOUT, 0};
                if (poll(&p, 1, 1000) <= 0) return false;
                continue;
            }
            done += static_cast<size_t>(n);
        }
        return true;
    }

private:
    long Available() {
        pollfd p{fd_, POLLIN, 0};
        int r = poll(&p, 1, 0);
        if (r < 0 || (p.revents & (POLLERR | POLLHUP | POLLNVAL))) return -1;
        return (p.revents & POLLIN) ? 1 : 0;
    }

    long ReadSome(char* buf, size_t len) {
        ssize_t n = recv(fd_, buf, len, 0);
        if (n == 0) return -1;  // closed
        if (n < 0) return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        return static_cast<long>(n);
    }

    int fd_ = -1;
#endif

public:
    // reads one frame, waiting up to timeout_ms for it to arrive
    bool ReadFrame(uint32_t& opcode, std::string& payload, int timeout_ms) {
        auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
        char header[8];
        if (!ReadExact(header, sizeof(header), deadline)) return false;
        uint32_t length = 0;
        opcode = 0;
        for (int i = 0; i < 4; i++) {
            opcode |= static_cast<uint32_t>(static_cast<unsigned char>(header[i])) << (8 * i);
            length |= static_cast<uint32_t>(static_cast<unsigned char>(header[4 + i])) << (8 * i);
        }
        if (length > 64 * 1024) return false;
        payload.assign(length, '\0');
        return ReadExact(payload.data(), length, deadline);
    }

    // discards replies Discord has sent; false if the connection is gone
    bool Drain() {
        char buf[1024];
        for (;;) {
            long avail = Available();
            if (avail < 0) return false;
            if (avail == 0) return true;
            long got = ReadSome(buf, std::min(sizeof(buf), static_cast<size_t>(avail)));
            if (got < 0) return false;
            if (got == 0) return true;
        }
    }

private:
    bool ReadExact(char* buf, size_t len, Clock::time_point deadline) {
        size_t got = 0;
        while (got < len) {
            long avail = Available();
            if (avail < 0) return false;
            if (avail == 0) {
                if (Clock::now() >= deadline) return false;
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            long n = ReadSome(buf + got, std::min(len - got, static_cast<size_t>(avail)));
            if (n < 0) return false;
            got += static_cast<size_t>(n);
        }
        return true;
    }
};

uint32_t ProcessId() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint32_t>(getpid());
#endif
}

std::atomic<bool> g_started{false};
std::thread g_thread;
std::mutex g_mutex;
std::condition_variable g_cv;
bool g_stop = false;
detail::Presence g_desired;
uint64_t g_version = 0;  // bumped on every state change

void Post(detail::Presence presence) {
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_desired = std::move(presence);
        g_version++;
    }
    g_cv.notify_one();
}

bool Connect(Connection& conn, bool& reported_missing) {
    if (!conn.Open()) {
        if (!reported_missing) {
            REXLOG_INFO("Discord: not running; retrying every {} s", kRetryInterval.count());
            reported_missing = true;
        }
        return false;
    }

    std::string handshake = std::string("{\"v\":1,\"client_id\":\"") + kClientId + "\"}";
    uint32_t opcode = 0;
    std::string reply;
    if (!conn.Write(detail::Frame(kOpHandshake, handshake)) ||
        !conn.ReadFrame(opcode, reply, kHandshakeTimeoutMs) ||
        opcode != kOpFrame || reply.find("\"READY\"") == std::string::npos) {
        REXLOG_WARN("Discord: handshake failed: {}", reply.empty() ? "no reply" : reply);
        conn.Close();
        return false;
    }

    REXLOG_INFO("Discord: connected");
    reported_missing = false;
    return true;
}

void Worker() {
    const uint32_t pid = ProcessId();
    Connection conn;
    uint64_t nonce = 0;
    uint64_t sent_version = 0;
    bool reported_missing = false;
    auto next_connect = Clock::now();
    auto next_send = Clock::now();

    std::unique_lock<std::mutex> lock(g_mutex);
    while (!g_stop) {
        auto now = Clock::now();

        if (!conn.IsOpen() && now >= next_connect) {
            lock.unlock();
            bool ok = Connect(conn, reported_missing);
            lock.lock();
            now = Clock::now();
            if (ok) sent_version = 0;  // resend the current state
            else next_connect = now + kRetryInterval;
        }

        if (conn.IsOpen()) {
            bool ok = conn.Drain();
            if (ok && sent_version != g_version && now >= next_send) {
                detail::Presence presence = g_desired;
                uint64_t version = g_version;
                lock.unlock();
                ok = conn.Write(detail::Frame(kOpFrame, detail::ActivityJson(presence, pid, ++nonce)));
                lock.lock();
                if (ok) {
                    sent_version = version;
                    next_send = now + kSendInterval;
                }
            }
            if (!ok) {
                REXLOG_INFO("Discord: connection lost; retrying every {} s", kRetryInterval.count());
                conn.Close();
                next_connect = now + kRetryInterval;
            }
        }

        // wake for new state, the next allowed send, a reconnect, or to drain replies
        auto wake = now + std::chrono::seconds(1);
        if (!conn.IsOpen()) wake = std::max(wake, next_connect);
        else if (sent_version != g_version) wake = std::min(wake, next_send);
        g_cv.wait_until(lock, wake);
    }

    if (conn.IsOpen()) {
        conn.Write(detail::Frame(kOpFrame, detail::ActivityJson({}, pid, ++nonce, true)));
        conn.Close();
    }
}

}

bool Enabled() {
    return g_started.load();
}

void Start() {
    if (g_started || !band3::settings::Startup().discord_enabled) return;
    g_stop = false;
    g_started = true;
    SetMenus();
    g_thread = std::thread(Worker);
}

void Stop() {
    if (!g_started) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_stop = true;
    }
    g_cv.notify_one();
    if (g_thread.joinable()) g_thread.join();
    g_started = false;
}

void SetMenus() {
    if (!Enabled()) return;
    Post(detail::Presence{});
}

void SetPlaying(std::string title, std::string artist, const events::BandInfo& band) {
    if (!Enabled()) return;
    detail::Presence presence;
    presence.playing = true;
    presence.title = detail::ToUtf8(title);
    presence.artist = detail::ToUtf8(artist);
    presence.band = detail::BandText(band);
    presence.start = static_cast<int64_t>(std::time(nullptr));
    Post(std::move(presence));
}

}
