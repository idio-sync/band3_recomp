#include "http_server.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <fmt/format.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include "src/config.h"
#include "src/Content/package_songs.h"
#include "src/game_writes.h"
#include "src/settings.h"
#include "src/Test/game_state.h"
#include "home_assistant.h"
#include "album_art.h"
#include "http_game.h"
#include "http_page.h"
#include "http_request.h"
#include "json.h"
#include "karaoke_page.h"
#include "live_events.h"
#include "local_address.h"
#include "lyrics.h"
#include "rhythmverse.h"
#include "song_downloads.h"
#include "web_client.h"

namespace band3::http {

namespace {

#ifdef _WIN32
using socket_t = SOCKET;
constexpr socket_t kNoSocket = INVALID_SOCKET;
void CloseSocket(socket_t s) { closesocket(s); }
#else
using socket_t = int;
constexpr socket_t kNoSocket = -1;
void CloseSocket(socket_t s) { close(s); }
#endif

// how often the server thread looks up from its socket to see if it should stop
constexpr std::chrono::milliseconds kStopCheck{200};
// how long a client has to send its request, and to take the reply
constexpr std::chrono::seconds kClientTimeout{3};
// how long a request waits for the game thread; loading screens hold it for a while
constexpr std::chrono::seconds kGameTimeout{5};
// connections served at once; more wait in the listen queue. Browsers open
// connections they may never use, so one idle client can't hold up the rest.
constexpr int kMaxClients = 8;
// /live/events' streams open at once, apart from kMaxClients; a fifth gets
// 503 and its page tries again
constexpr int kMaxStreams = 4;
// how often a stream looks at the game's state and sends what changed
constexpr std::chrono::milliseconds kStreamTick{250};
// a request line and headers longer than this aren't from a browser
constexpr size_t kMaxHead = 16 * 1024;
// the page's POSTs are a line of JSON
constexpr size_t kMaxBody = 4 * 1024;
// the files served from game:\, as RB3E serves them from its rawfiles
constexpr uintmax_t kMaxFile = 4 * 1024 * 1024;

constexpr std::string_view kText = "text/plain; charset=utf-8";

// Game work: a request's server thread queues it, and the game thread runs it
// at its next frame. A job that hasn't started when the request gives up is
// dropped, so a jump or a script never happens long after its reply said it
// didn't.
using GameJob = std::function<void(PPCContext&, uint8_t*)>;

struct PendingJob {
    GameJob run;
    bool started = false;
    bool done = false;
    bool cancelled = false;
};

std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;
std::deque<std::shared_ptr<PendingJob>> g_jobs;
std::atomic<bool> g_has_jobs{false};
bool g_stopping = false;

// false when the game didn't get to it in kGameTimeout, or band3 is closing
bool RunOnGameThread(GameJob run) {
    auto job = std::make_shared<PendingJob>();
    job->run = std::move(run);
    std::unique_lock lock(g_jobs_mutex);
    if (g_stopping) return false;
    g_jobs.push_back(job);
    g_has_jobs = true;
    g_jobs_cv.wait_for(lock, kGameTimeout, [&] { return job->done || g_stopping; });
    if (job->done) return true;
    // once started it finishes, and its results are the request's
    if (job->started) {
        g_jobs_cv.wait(lock, [&] { return job->done; });
        return true;
    }
    job->cancelled = true;
    return false;
}

// a file at game:\, found as the game finds it: what it wrote (in the user data
// root) first, then the game data
std::optional<std::string> ReadGameFile(const char* name) {
    std::vector<std::filesystem::path> roots;
    if (auto* runtime = rex::Runtime::instance()) {
        roots.push_back(band3::GameWritesFolder(runtime->user_data_root()));
    }
    roots.push_back(band3::GameDataRoot());
    for (const auto& root : roots) {
        std::error_code ec;
        const std::filesystem::path path = root / name;
        const uintmax_t size = std::filesystem::file_size(path, ec);
        if (ec || size > kMaxFile) continue;
        std::ifstream file(path, std::ios::binary);
        if (!file) continue;
        std::ostringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }
    return std::nullopt;
}

// what the hooks have kept (src/Test/game_state.h), so /status never waits on
// the game thread
Status CurrentStatus() {
    const test::GameStateSnapshot game = test::GameState::Get().Snapshot();
    Status status;
    status.screen = game.screen;
    // the Music Library's screen; /jump checks its panel is up as well
    status.in_library = game.screen == "song_select_screen";
    status.ha_state = ha::StateName();
    if (game.in_game) {
        status.playing = Status::Playing{game.song_shortname, game.song_name, game.song_artist,
                                         game.score, game.song_ms, game.song_length_ms};
    }
    return status;
}

// /song_details as last built, and the songs (by ID) it was built from. Its
// game work goes a chunk of songs a frame, so a library of thousands of custom
// songs can't hold up a frame of the game.
constexpr size_t kDetailsChunk = 100;
std::mutex g_details_mutex;
std::vector<int32_t> g_details_ids;
std::string g_details_json;

// nullopt when the game is busy
std::optional<std::string> SongDetailsJson() {
    std::lock_guard lock(g_details_mutex);
    std::vector<int32_t> ids;
    if (!RunOnGameThread([&ids](PPCContext& ctx, uint8_t* base) {
            ids = game::RankedIds(ctx, base);
        })) {
        return std::nullopt;
    }
    if (!g_details_json.empty() && ids == g_details_ids) return g_details_json;

    std::vector<SongDetails> details;
    details.reserve(ids.size());
    for (size_t at = 0; at < ids.size(); at += kDetailsChunk) {
        const std::vector<int32_t> chunk(ids.begin() + at,
                                         ids.begin() + std::min(at + kDetailsChunk, ids.size()));
        std::vector<SongDetails> part;
        if (!RunOnGameThread([&part, &chunk](PPCContext& ctx, uint8_t* base) {
                part = game::Details(ctx, base, chunk);
            })) {
            return std::nullopt;
        }
        std::move(part.begin(), part.end(), std::back_inserter(details));
    }
    g_details_json = FormatSongDetails(details);
    g_details_ids = std::move(ids);
    return g_details_json;
}

std::string Busy(bool cors) {
    return Response(503, kText, "The game is busy, try again", cors);
}

// The game's songs with their IDs, for /library/duplicates, read a chunk a
// frame as /song_details' are, and kept while the list of IDs is the same
std::mutex g_game_songs_mutex;
std::vector<int32_t> g_game_song_ids;
std::vector<content::GameSong> g_game_songs;

// nullopt when the game is busy
std::optional<std::vector<content::GameSong>> GameSongs() {
    std::lock_guard lock(g_game_songs_mutex);
    std::vector<int32_t> ids;
    if (!RunOnGameThread([&ids](PPCContext& ctx, uint8_t* base) { ids = game::RankedIds(ctx, base); })) {
        return std::nullopt;
    }
    if (!g_game_song_ids.empty() && ids == g_game_song_ids) return g_game_songs;
    std::vector<content::GameSong> songs;
    for (size_t at = 0; at < ids.size(); at += kDetailsChunk) {
        const std::vector<int32_t> chunk(ids.begin() + at,
                                         ids.begin() + std::min(at + kDetailsChunk, ids.size()));
        if (!RunOnGameThread([&songs, &chunk](PPCContext& ctx, uint8_t* base) {
                for (const int32_t id : chunk) {
                    if (auto song = game::Song(ctx, base, id)) {
                        songs.push_back({song->shortname, id, song->title, song->artist});
                    }
                }
            })) {
            return std::nullopt;
        }
    }
    g_game_songs = std::move(songs);
    g_game_song_ids = std::move(ids);
    return g_game_songs;
}

// /library/duplicates: the packages' songs, read in the background the first
// time (the page asks again until they're read), and the game's own
std::string LibraryDuplicates(bool cors) {
    content::PackageSongsSnapshot snapshot = content::CurrentPackageSongs();
    std::vector<content::GameSong> game;
    // not while they're read: the page is asking every second then. None at
    // all is the song manager not done loading, just after the game starts.
    if (!snapshot.status.reading) {
        if (auto songs = GameSongs(); songs && !songs->empty()) {
            game = std::move(*songs);
            snapshot.status.game = true;
        }
    }
    const auto groups = content::FindDuplicates(snapshot.packages, game, snapshot.left_out);
    return Response(200, "application/json",
                    content::FormatDuplicates(groups, snapshot.packages, snapshot.status,
                                              snapshot.set_aside),
                    cors);
}

// POST /library/set_aside {"file": ...} or {"left_out": true}, and POST
// /library/put_back {"file": ...}: JSON only, as /rv/download, so another
// site's page can't send one. Nothing changes in the game until it next starts.
std::string LibrarySetAside(const Request& request, bool put_back, bool cors) {
    if (!request.content_type.starts_with("application/json")) {
        return Response(415, kText, "Send the request as JSON", cors);
    }
    const auto body = json::Parse(request.body);
    const std::string file = body ? (*body)["file"].Text() : std::string();
    std::string problem;
    std::string done;
    if (put_back) {
        problem = content::PutBackPackage(file);
        done = "Put back";
    } else if (body && (*body)["left_out"].Bool()) {
        size_t count = 0;
        problem = content::SetAsideLeftOut(count);
        done = fmt::format("{} set aside at the next launch", count);
    } else {
        problem = content::SetAsidePackage(file);
        done = "Set aside at the next launch";
    }
    if (!problem.empty()) return Response(409, kText, problem, cors);
    return Response(200, kText, done, cors);
}

// Album art sent so far, as JPEGs by shortname, so a page scrolled back and
// forth reads each song's art from the game once. A song without any isn't
// kept: the song manager may not have finished loading when it was asked.
// About 15 KB each; past kMaxArt, the oldest go first.
constexpr size_t kMaxArt = 512;
// browsers keep it for this long (seconds) before asking again
constexpr int kArtMaxAge = 3600;
std::mutex g_art_mutex;
std::unordered_map<std::string, std::string> g_art;
std::deque<std::string> g_art_order;
// Reading one takes the game thread about half a millisecond, and a page asks
// for a screenful at once, so they're read one at a time: one a frame at most.
std::mutex g_art_read_mutex;

std::optional<std::string> CachedArt(const std::string& shortname) {
    std::lock_guard lock(g_art_mutex);
    const auto it = g_art.find(shortname);
    if (it == g_art.end()) return std::nullopt;
    return it->second;
}

void CacheArt(const std::string& shortname, const std::string& jpeg) {
    std::lock_guard lock(g_art_mutex);
    if (!g_art.emplace(shortname, jpeg).second) return;
    g_art_order.push_back(shortname);
    if (g_art_order.size() > kMaxArt) {
        g_art.erase(g_art_order.front());
        g_art_order.pop_front();
    }
}

// the song's album art as a JPEG; nullopt for a song without any, or when the
// game is busy (`busy` says which)
std::optional<std::string> AlbumArt(const std::string& shortname, bool& busy) {
    busy = false;
    if (auto jpeg = CachedArt(shortname)) return jpeg;
    std::lock_guard lock(g_art_read_mutex);
    // another request may have read it while this one waited
    if (auto jpeg = CachedArt(shortname)) return jpeg;
    std::optional<std::string> file;
    if (!RunOnGameThread([&file, &shortname](PPCContext& ctx, uint8_t* base) {
            file = game::AlbumArtFile(ctx, base, shortname);
        })) {
        busy = true;
        return std::nullopt;
    }
    // decoded here, so the game thread only reads the file
    std::optional<Image> image = file ? DecodeXboxBitmap(*file) : std::nullopt;
    if (!image) {
        if (file) REXLOG_DEBUG("Web server: {}'s album art isn't DXT1 or DXT5", shortname);
        return std::nullopt;
    }
    std::string jpeg = EncodeJpeg(*image);
    CacheArt(shortname, jpeg);
    return jpeg;
}

// /lyrics' replies by shortname, newest last, for kMaxLyrics songs. An empty
// reply is a song without lyrics band3 can show (404); parsing happens here,
// on the request's thread, so the game thread only reads the file.
constexpr size_t kMaxLyrics = 8;
std::mutex g_lyrics_mutex;
std::deque<std::pair<std::string, std::string>> g_lyrics;

// nullopt when the song has none, or the game is busy (busy says which)
std::optional<std::string> LyricsJson(const std::string& shortname, bool& busy) {
    {
        std::lock_guard lock(g_lyrics_mutex);
        for (const auto& [name, json] : g_lyrics) {
            if (name == shortname) return json.empty() ? std::nullopt : std::optional(json);
        }
    }
    std::optional<std::string> file;
    if (!RunOnGameThread([&file, &shortname](PPCContext& ctx, uint8_t* base) {
            file = game::MidiFile(ctx, base, shortname);
        })) {
        busy = true;
        return std::nullopt;
    }
    std::string json;
    if (file) {
        if (const auto midi = lyrics::ReadMidi(*file)) {
            const auto parts = lyrics::FromMidi(*midi);
            if (!parts.empty()) json = lyrics::FormatJson(shortname, parts);
        } else {
            REXLOG_WARN("Web server: {}'s MIDI file isn't one band3 can read", shortname);
        }
    }
    std::lock_guard lock(g_lyrics_mutex);
    g_lyrics.emplace_back(shortname, json);
    if (g_lyrics.size() > kMaxLyrics) g_lyrics.pop_front();
    return json.empty() ? std::nullopt : std::optional(json);
}

// the song IDs of every song in the game; nullopt while the game is busy
std::optional<std::set<int32_t>> GameSongIds() {
    std::vector<int32_t> ids;
    if (!RunOnGameThread([&ids](PPCContext& ctx, uint8_t* base) {
            ids = game::RankedIds(ctx, base);
        })) {
        return std::nullopt;
    }
    return std::set<int32_t>(ids.begin(), ids.end());
}

// /rv/search: a page of RhythmVerse's songs, asked of it as this request waits
std::string RhythmVerseSearch(const Request& request, bool cors) {
    const auto options = rhythmverse::ParseSearchOptions(request.target);
    const auto search = rhythmverse::Search(options);
    const web::Reply reply = web::PostForm(search.url, search.form);
    if (!reply.error.empty()) {
        return Response(502, kText, "Couldn't reach RhythmVerse: " + reply.error, cors);
    }
    if (reply.status != 200) {
        return Response(502, kText, fmt::format("RhythmVerse answered {}", reply.status), cors);
    }
    auto result = rhythmverse::ParseSearch(reply.body);
    if (!result) {
        REXLOG_WARN("Web server: RhythmVerse's search reply wasn't as expected: {:.200}", reply.body);
        return Response(502, kText, "RhythmVerse's reply wasn't one band3 can read", cors);
    }
    result->page_size = search.page_size;
    // what RhythmVerse has now of songs band3 downloaded, for /rv/updates
    rhythmverse::NoteSeen(result->songs);
    // RhythmVerse can't leave them out itself
    if (options.downloadable_only) {
        std::erase_if(result->songs, [](const auto& s) { return s.download_url.empty(); });
    }
    rhythmverse::Remember(result->songs);

    rhythmverse::LocalSongs local;
    local.files = rhythmverse::LocalFiles();
    local.records = rhythmverse::Records();
    // the game's songs say what's in it whatever their files are called; while
    // the game is busy, the page goes by artist and title
    local.game_ids = GameSongIds();
    return Response(200, "application/json", rhythmverse::FormatSearch(*result, local), cors);
}

// POST /rv/download {"file_id": ...}. JSON only, so another site's page can't
// send one without the preflight band3 never answers.
std::string RhythmVerseDownload(const Request& request, bool cors) {
    if (!request.content_type.starts_with("application/json")) {
        return Response(415, kText, "Send the file ID as JSON", cors);
    }
    const auto body = json::Parse(request.body);
    const std::string file_id = body ? (*body)["file_id"].Text() : std::string();
    const bool update = body && (*body)["update"].Bool();
    switch (rhythmverse::QueueDownload(file_id, update)) {
        case rhythmverse::QueueResult::kQueued:
            return Response(200, kText, update ? "Updating" : "Downloading", cors);
        case rhythmverse::QueueResult::kNoUpdate:
            return Response(409, kText, "RhythmVerse has nothing newer of it", cors);
        case rhythmverse::QueueResult::kHave:
            return Response(200, kText, "Already in the song folders", cors);
        case rhythmverse::QueueResult::kUnknown:
            break;
        case rhythmverse::QueueResult::kNotHosted:
            return Response(409, kText,
                            "RhythmVerse doesn't host this one: download it from its page", cors);
        case rhythmverse::QueueResult::kNoFolder:
            return Response(409, kText, "content_folders names no folder to download to", cors);
    }
    return Response(404, kText, "No search has found that song; search for it again", cors);
}

// /rv/updates: what the records say RhythmVerse has newer versions of
std::string RhythmVerseUpdates(bool cors) {
    rhythmverse::LocalSongs local;
    local.records = rhythmverse::Records();
    local.files = rhythmverse::LocalFiles();
    // the game asked whether it has them only when there are some to say it of
    const bool any = std::ranges::any_of(local.records, [&](const auto& entry) {
        return rhythmverse::RecordUpdate(entry.first, local.records) != rhythmverse::UpdateState::kNone;
    });
    if (any) local.game_ids = GameSongIds();
    return Response(200, "application/json",
                    rhythmverse::FormatUpdates(local, rhythmverse::CheckStatus()), cors);
}

std::string Handle(const Request& request) {
    const bool cors = REXCVAR_GET(http_allow_cors);
    const Route route = MatchRoute(request.target);
    // POST for what changes things beyond the game, GET for the rest
    const bool post = route.endpoint == Endpoint::kRvDownload || route.endpoint == Endpoint::kRvCheck ||
                      route.endpoint == Endpoint::kSetAside || route.endpoint == Endpoint::kPutBack;
    if (request.method != (post ? "POST" : "GET")) {
        return Response(405, kText, post ? "Only POST is supported" : "Only GET is supported", cors);
    }
    const bool rhythmverse = route.endpoint == Endpoint::kRvSearch ||
                             route.endpoint == Endpoint::kRvDownload ||
                             route.endpoint == Endpoint::kRvDownloads ||
                             route.endpoint == Endpoint::kRvUpdates ||
                             route.endpoint == Endpoint::kRvCheck;
    if (rhythmverse && !REXCVAR_GET(http_rhythmverse)) {
        return Response(403, kText, "RhythmVerse is off (http_rhythmverse)", cors);
    }

    switch (route.endpoint) {
        case Endpoint::kIndex: {
            // a page of the user's own at game:\ replaces band3's, as it does RB3E's
            if (auto page = ReadGameFile("rb3e_index.html")) {
                return Response(200, "text/html; charset=utf-8", *page, cors);
            }
            return Response(200, "text/html; charset=utf-8", kIndexPage, cors);
        }
        case Endpoint::kSong: {
            std::optional<SongInfo> song;
            const int32_t id = route.song_id;
            if (!RunOnGameThread([&song, id](PPCContext& ctx, uint8_t* base) {
                    song = game::Song(ctx, base, id);
                })) {
                return Busy(cors);
            }
            if (!song) return Response(404, kText, "Not Found", cors);
            return Response(200, kText, FormatSong(*song, false), cors);
        }
        case Endpoint::kListSongs: {
            std::vector<SongInfo> songs;
            if (!RunOnGameThread([&songs](PPCContext& ctx, uint8_t* base) {
                    songs = game::RankedSongs(ctx, base);
                })) {
                return Busy(cors);
            }
            std::string body;
            for (const SongInfo& song : songs) body += FormatSong(song, true);
            return Response(200, kText, body, cors);
        }
        case Endpoint::kJump: {
            auto result = game::JumpResult::kNotInLibrary;
            const std::string& shortname = route.argument;
            if (!RunOnGameThread([&result, &shortname](PPCContext& ctx, uint8_t* base) {
                    result = game::JumpToSong(ctx, base, shortname);
                })) {
                return Busy(cors);
            }
            switch (result) {
                case game::JumpResult::kJumped:
                    return Response(200, kText, "OK", cors);
                case game::JumpResult::kNotInLibrary:
                    return Response(409, kText, "Open the Music Library first", cors);
                case game::JumpResult::kUnknownSong:
                    break;
            }
            return Response(404, kText, "No song has that shortname", cors);
        }
        case Endpoint::kExecute: {
            if (!REXCVAR_GET(http_allow_scripts)) {
                return Response(403, kText, "Scripts are off (http_allow_scripts)", cors);
            }
            REXLOG_INFO("Web server: running script {}", route.argument);
            const std::string& script = route.argument;
            if (!RunOnGameThread([&script](PPCContext& ctx, uint8_t* base) {
                    game::ExecuteScript(ctx, base, script);
                })) {
                return Busy(cors);
            }
            return Response(200, kText, "OK", cors);
        }
        case Endpoint::kJsonRpc: {
            if (auto json = ReadGameFile("discordrp.json")) {
                return Response(200, "application/json", *json, cors);
            }
            break;
        }
        case Endpoint::kStatus:
            return Response(200, "application/json", FormatStatus(CurrentStatus()), cors);
        case Endpoint::kSongDetails: {
            if (auto json = SongDetailsJson()) {
                return Response(200, "application/json", *json, cors);
            }
            return Busy(cors);
        }
        case Endpoint::kDuplicates:
            return LibraryDuplicates(cors);
        case Endpoint::kSetAside:
        case Endpoint::kPutBack:
            return LibrarySetAside(request, route.endpoint == Endpoint::kPutBack, cors);
        case Endpoint::kAlbumArt: {
            bool busy = false;
            if (auto jpeg = AlbumArt(route.argument, busy)) {
                return Response(200, "image/jpeg", *jpeg, cors, kArtMaxAge);
            }
            if (busy) return Busy(cors);
            return Response(404, kText, "No album art for that shortname", cors);
        }
        case Endpoint::kRvSearch:
            return RhythmVerseSearch(request, cors);
        case Endpoint::kRvDownload:
            return RhythmVerseDownload(request, cors);
        case Endpoint::kRvDownloads: {
            const auto downloads = rhythmverse::Downloads();
            // whether the game has taken the downloaded ones in yet
            const bool done = std::ranges::any_of(downloads, [](const auto& d) {
                return d.state == rhythmverse::Download::State::kDone;
            });
            return Response(200, "application/json",
                            rhythmverse::FormatDownloads(
                                downloads, rex::path_to_utf8(rhythmverse::DownloadFolder()),
                                done ? GameSongIds() : std::nullopt),
                            cors);
        }
        case Endpoint::kRvUpdates:
            return RhythmVerseUpdates(cors);
        case Endpoint::kRvCheck:
            // JSON only, as /rv/download: another site's page can't ask
            if (!request.content_type.starts_with("application/json")) {
                return Response(415, kText, "Send the request as JSON", cors);
            }
            if (rhythmverse::DownloadFolder().empty()) {
                return Response(409, kText, "content_folders names no folder to download to", cors);
            }
            rhythmverse::CheckForUpdates(false);
            return Response(200, kText, "Checking", cors);
        case Endpoint::kKaraoke:
            return Response(200, "text/html; charset=utf-8", kKaraokePage, cors);
        case Endpoint::kKaraokeModel:
            return Response(200, "text/javascript; charset=utf-8", kKaraokeModel, cors);
        case Endpoint::kLyrics: {
            bool busy = false;
            if (auto json = LyricsJson(route.argument, busy)) {
                return Response(200, "application/json", *json, cors);
            }
            if (busy) return Busy(cors);
            return Response(404, kText, "No lyrics for that shortname", cors);
        }
        case Endpoint::kLiveEvents:
            break;  // Serve streams it before Handle; a POST lands here as a 405 first
        case Endpoint::kNotFound:
            break;
    }
    return Response(404, kText, "Not Found", cors);
}

bool SendAll(socket_t s, const std::string& data) {
#ifdef _WIN32
    constexpr int kFlags = 0;
#else
    // a client that goes away mid-reply mustn't SIGPIPE the game
    constexpr int kFlags = MSG_NOSIGNAL;
#endif
    size_t sent = 0;
    while (sent < data.size()) {
        const int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), kFlags);
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

void SetTimeouts(socket_t s) {
#ifdef _WIN32
    const DWORD ms = static_cast<DWORD>(
        std::chrono::duration_cast<std::chrono::milliseconds>(kClientTimeout).count());
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&ms), sizeof(ms));
#else
    timeval tv{static_cast<time_t>(kClientTimeout.count()), 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

class Server {
public:
    static Server& Get() {
        static Server server;
        return server;
    }

    void Start() {
        const auto& startup = band3::settings::Startup();
        if (thread_.joinable() || !startup.http_enabled) return;
        const int32_t port = startup.http_port;
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(static_cast<uint16_t>(port));
        if (inet_pton(AF_INET, startup.http_address.c_str(), &addr.sin_addr) != 1) {
            REXLOG_WARN("Web server: http_address '{}' isn't an IPv4 address, the server is off",
                        startup.http_address);
            return;
        }
#ifdef _WIN32
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            REXLOG_WARN("Web server: WSAStartup failed, the server is off");
            return;
        }
#endif
        listener_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (listener_ == kNoSocket) {
            REXLOG_WARN("Web server: couldn't make a socket, the server is off");
            return;
        }
#ifndef _WIN32
        // so a restarted game can take the port straight back
        int reuse = 1;
        setsockopt(listener_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
        if (bind(listener_, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr)) != 0 ||
            listen(listener_, 8) != 0) {
            REXLOG_WARN("Web server: port {} is in use or unavailable, the server is off", port);
            CloseSocket(listener_);
            listener_ = kNoSocket;
            return;
        }
        {
            std::lock_guard lock(g_jobs_mutex);
            g_stopping = false;
        }
        stopping_ = false;
        thread_ = std::thread([this] { Run(); });
        // on every address, the one other devices reach it at
        std::string shown = startup.http_address;
        if (addr.sin_addr.s_addr == htonl(INADDR_ANY)) {
            const std::string local = band3::net::LocalAddress();
            shown = local.empty() ? "localhost" : local;
        }
        REXLOG_INFO("Web server: listening on {}:{}, open http://{}:{}/",
                    startup.http_address, port, shown, port);
        // newer versions of what band3 downloaded, for the RhythmVerse tab
        if (REXCVAR_GET(http_rhythmverse)) rhythmverse::CheckForUpdates(true);
    }

    void Stop() {
        if (!thread_.joinable()) return;
        stopping_ = true;
        {
            std::lock_guard lock(g_jobs_mutex);
            g_stopping = true;
            for (const auto& job : g_jobs) job->cancelled = true;
            g_jobs.clear();
            g_has_jobs = false;
        }
        g_jobs_cv.notify_all();
        thread_.join();
        // clients give up within kClientTimeout, and their game work is cancelled;
        // only one whose work the game thread stopped in the middle of stays, and
        // band3 closes without it
        {
            std::unique_lock lock(clients_mutex_);
            if (!clients_cv_.wait_for(lock, kClientTimeout + kGameTimeout,
                                      [this] { return clients_ == 0 && streams_ == 0; })) {
                REXLOG_WARN("Web server: {} request(s) still running at shutdown",
                            clients_ + streams_);
            }
        }
        CloseSocket(listener_);
        listener_ = kNoSocket;
    }

private:
    void Run() {
        while (!stopping_) {
            fd_set read;
            FD_ZERO(&read);
            FD_SET(listener_, &read);
            timeval timeout{0, static_cast<long>(kStopCheck.count() * 1000)};
#ifdef _WIN32
            const int nfds = 0;  // ignored on Windows
#else
            const int nfds = listener_ + 1;
#endif
            const int n = select(nfds, &read, nullptr, nullptr, &timeout);
            if (n < 0) {
                REXLOG_WARN("Web server: select failed, the server stops");
                break;
            }
            if (n == 0) continue;
            {
                // at the limit, leave the connection queued until a client finishes
                std::unique_lock lock(clients_mutex_);
                if (!clients_cv_.wait_for(lock, kStopCheck,
                                          [this] { return clients_ < kMaxClients; })) {
                    continue;
                }
            }
            socket_t client = accept(listener_, nullptr, nullptr);
            if (client == kNoSocket) continue;
            {
                std::lock_guard lock(clients_mutex_);
                clients_++;
            }
            std::thread([this, client] {
                Serve(client);
                CloseSocket(client);
                // notified under the lock, so Stop can't return (and the
                // server go) before this thread is done with it
                std::lock_guard lock(clients_mutex_);
                clients_--;
                clients_cv_.notify_all();
            }).detach();
        }
    }

    // one request per connection, as RB3E serves them
    void Serve(socket_t client) {
        SetTimeouts(client);
        std::string head;
        while (head.find("\r\n\r\n") == std::string::npos) {
            if (head.size() > kMaxHead || stopping_) return;
            char buf[1024];
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return;
            head.append(buf, static_cast<size_t>(n));
        }
        const size_t head_end = head.find("\r\n\r\n") + 4;
        std::optional<Request> request = ParseRequest(std::string_view(head).substr(0, head_end));
        if (!request) {
            SendAll(client, Response(400, kText, "Bad Request", false));
            return;
        }
        if (request->content_length > kMaxBody) {
            SendAll(client, Response(413, kText, "Too much was sent", false));
            return;
        }
        request->body = head.substr(head_end);
        while (request->body.size() < request->content_length) {
            if (stopping_) return;
            char buf[1024];
            const int n = recv(client, buf, sizeof(buf), 0);
            if (n <= 0) return;
            request->body.append(buf, static_cast<size_t>(n));
        }
        request->body.resize(request->content_length);
        REXLOG_DEBUG("Web server: {} {}", request->method, request->target);
        if (request->method == "GET" &&
            MatchRoute(request->target).endpoint == Endpoint::kLiveEvents) {
            ServeStream(client);
            return;
        }
        SendAll(client, Handle(*request));
    }

    // /live/events: this connection's thread writes the stream until the
    // client goes (a send fails, or times out after kClientTimeout) or the
    // server stops. It gives up its request slot for one of kMaxStreams.
    void ServeStream(socket_t client) {
        const bool cors = REXCVAR_GET(http_allow_cors);
        bool full;
        {
            std::lock_guard lock(clients_mutex_);
            full = streams_ >= kMaxStreams;
            if (!full) {
                streams_++;
                clients_--;
            }
            clients_cv_.notify_all();
        }
        if (full) {
            SendAll(client, Response(503, kText, "Too many live pages are open", cors));
            return;
        }
        REXLOG_DEBUG("Web server: a live page connected");
        live::Ticker ticker;
        bool open = SendAll(client, live::StreamHead(cors));
        while (open && !stopping_) {
            const test::GameStateSnapshot game = test::GameState::Get().Snapshot();
            const std::string out =
                ticker.Tick(live::FromSnapshot(game), game.song_ms, live::Ticker::Clock::now());
            if (!out.empty()) open = SendAll(client, out);
            if (open) std::this_thread::sleep_for(kStreamTick);
        }
        REXLOG_DEBUG("Web server: a live page left");
        // back among the clients, which the connection's thread counts out
        std::lock_guard lock(clients_mutex_);
        streams_--;
        clients_++;
    }

    socket_t listener_ = kNoSocket;
    std::thread thread_;
    std::atomic<bool> stopping_{false};
    // the connections being served, each on a thread of its own
    std::mutex clients_mutex_;
    std::condition_variable clients_cv_;
    int clients_ = 0;
    int streams_ = 0;  // of the connections, those streaming /live/events
};

}

void StartServer() { Server::Get().Start(); }

void StopServer() {
    Server::Get().Stop();
    rhythmverse::StopDownloads();
}

bool Enabled() { return band3::settings::Startup().http_enabled; }

void RunGameJobs(PPCContext& ctx, uint8_t* base) {
    if (!g_has_jobs.load(std::memory_order_acquire)) return;
    std::deque<std::shared_ptr<PendingJob>> jobs;
    {
        std::lock_guard lock(g_jobs_mutex);
        jobs.swap(g_jobs);
        g_has_jobs = false;
        for (const auto& job : jobs) job->started = !job->cancelled;
    }
    for (const auto& job : jobs) {
        if (!job->started) continue;
        job->run(ctx, base);
        std::lock_guard lock(g_jobs_mutex);
        job->done = true;
    }
    g_jobs_cv.notify_all();
}

}
