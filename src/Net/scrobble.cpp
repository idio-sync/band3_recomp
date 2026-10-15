#include "scrobble.h"

#include <SDL3/SDL_misc.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include "src/Launcher/config_file.h"
#include "src/Net/http_request.h"
#include "src/Net/scrobble_protocol.h"
#include "src/Net/web_client.h"
#include "src/settings.h"

#ifndef BAND3_LASTFM_API_KEY
#define BAND3_LASTFM_API_KEY ""
#endif
#ifndef BAND3_LASTFM_API_SECRET
#define BAND3_LASTFM_API_SECRET ""
#endif

namespace band3::scrobble {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::string_view kLastfmApiKey = BAND3_LASTFM_API_KEY;
constexpr std::string_view kLastfmApiSecret = BAND3_LASTFM_API_SECRET;

// how often to ask Last.fm whether the player has allowed band3 yet, and for
// how long (its tokens last an hour)
constexpr auto kAuthPoll = 5s;
constexpr auto kAuthWait = 10min;
// waits before sending listens again after a failure: doubling, up to the most
constexpr auto kFirstRetry = 1min;
constexpr auto kMostRetry = 30min;
// listens kept for each service while it can't be reached
constexpr size_t kMostPending = 1000;
// ListenBrainz listens to an "import" (its limit is far above)
constexpr size_t kListenBrainzBatch = 100;
constexpr int kListenBrainzTimeoutMs = 15000;
// how long Stop waits on a request in flight before leaving it be
constexpr auto kStopWait = 2s;

struct Job {
    bool listen = false;  // a listen to send; otherwise "now playing"
    Track track;
};

// one service's listens waiting to be sent
struct Queue {
    bool on = false;
    std::deque<Track> pending;
    Clock::time_point next_send{};
    Clock::duration retry = kFirstRetry;

    void Add(const Track& track) {
        if (!on) return;
        pending.push_back(track);
        while (pending.size() > kMostPending) pending.pop_front();
    }
    void Failed(Clock::time_point now) {
        next_send = now + retry;
        retry = std::min<Clock::duration>(retry * 2, kMostRetry);
    }
    void Sent(size_t count) {
        pending.erase(pending.begin(), pending.begin() + std::min(count, pending.size()));
        retry = kFirstRetry;
    }
};

// shared by the game thread and the worker; the worker keeps its own hold, so
// it can be left to finish a request after Stop
struct State {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    bool done = false;
    std::deque<Job> jobs;
    // the song on now, from the game thread
    std::optional<Track> playing;
};

std::mutex g_control;
std::shared_ptr<State> g_state;
std::thread g_thread;
std::atomic<bool> g_started{false};

struct Config {
    std::filesystem::path config_path;
    bool lastfm = false;
    std::string lastfm_session;
    std::string lastfm_url;
    bool listenbrainz = false;
    std::string listenbrainz_token;
    std::string listenbrainz_url;
};

std::string Describe(const web::Reply& reply, const std::string& message = {}) {
    if (!message.empty()) return message;
    if (reply.status == 0) return reply.error.empty() ? "no reply" : reply.error;
    return "HTTP " + std::to_string(reply.status);
}

// the worker; nothing it does holds the game thread
class Worker {
public:
    Worker(std::shared_ptr<State> state, Config config)
        : state_(std::move(state)), config_(std::move(config)) {
        lastfm_.on = config_.lastfm;
        listenbrainz_.on = config_.listenbrainz;
    }

    void Run() {
        if (lastfm_.on && config_.lastfm_session.empty()) BeginLastfmAuth();

        std::unique_lock lock(state_->mutex);
        while (!state_->stop) {
            std::deque<Job> jobs;
            jobs.swap(state_->jobs);
            lock.unlock();

            for (const Job& job : jobs) {
                if (job.listen) {
                    lastfm_.Add(job.track);
                    listenbrainz_.Add(job.track);
                } else {
                    NowPlaying(job.track);
                }
            }
            const auto now = Clock::now();
            if (authorizing_ && now >= next_auth_poll_) PollLastfmAuth(now);
            if (Stopping()) break;
            if (lastfm_.on && !authorizing_ && !lastfm_.pending.empty() && now >= lastfm_.next_send) {
                SendLastfm(now);
            }
            if (Stopping()) break;
            if (listenbrainz_.on && !listenbrainz_.pending.empty() &&
                now >= listenbrainz_.next_send) {
                SendListenBrainz(now);
            }

            lock.lock();
            if (state_->stop || !state_->jobs.empty()) continue;
            state_->cv.wait_until(lock, NextWake());
        }
        if (!lock.owns_lock()) lock.lock();
        state_->done = true;
        state_->cv.notify_all();
    }

private:
    bool Stopping() {
        std::lock_guard lock(state_->mutex);
        return state_->stop;
    }

    Clock::time_point NextWake() const {
        auto wake = Clock::now() + 1min;
        if (authorizing_) wake = std::min(wake, next_auth_poll_);
        if (lastfm_.on && !authorizing_ && !lastfm_.pending.empty()) {
            wake = std::min(wake, lastfm_.next_send);
        }
        if (listenbrainz_.on && !listenbrainz_.pending.empty()) {
            wake = std::min(wake, listenbrainz_.next_send);
        }
        return wake;
    }

    // Every request ends with a check of Stopping(): past Stop the log may be
    // gone, so a worker left finishing a request says nothing more.
    web::Reply CallLastfm(const Params& params) {
        return web::PostForm(config_.lastfm_url, LastfmForm(params, kLastfmApiSecret), 64 * 1024);
    }

    // the session is no good: stop scrobbling to Last.fm, and forget it, so the
    // next start asks the player again
    void DropLastfmSession(const std::string& why) {
        REXLOG_WARN("Scrobbling: Last.fm refused band3's session ({}); band3 will ask for "
                    "permission again at the next start",
                    why);
        lastfm_.on = false;
        lastfm_.pending.clear();
        SaveSession("");
    }

    void SaveSession(const std::string& session) {
        config_.lastfm_session = session;
        // for the settings, so F4's Save keeps it
        rex::cvar::SetFlagByName("lastfm_session", session);
        const launcher::ConfigEdit edit{
            "lastfm_session",
            session.empty() ? std::nullopt : std::optional<launcher::ConfigValue>(session)};
        const launcher::SaveResult result =
            launcher::SaveConfigFile(config_.config_path, std::span(&edit, 1));
        if (!result.ok) {
            REXLOG_WARN("Scrobbling: couldn't save the Last.fm session to {}: {}",
                        config_.config_path.string(), result.error);
        }
    }

    void BeginLastfmAuth() {
        const web::Reply reply = CallLastfm(LastfmGetToken(kLastfmApiKey));
        if (Stopping()) return;
        std::string message;
        const Outcome outcome = LastfmOutcome(reply.status, reply.body, &message);
        const std::string token = outcome == Outcome::kOk ? LastfmToken(reply.body) : std::string();
        if (token.empty()) {
            REXLOG_WARN("Scrobbling: couldn't start signing in to Last.fm ({}); Last.fm is off "
                        "until the next start",
                        Describe(reply, message));
            lastfm_.on = false;
            lastfm_.pending.clear();
            return;
        }
        token_ = token;
        authorizing_ = true;
        auth_deadline_ = Clock::now() + kAuthWait;
        next_auth_poll_ = Clock::now() + kAuthPoll;
        const std::string url = LastfmAuthUrl(kLastfmApiKey, token_);
        REXLOG_INFO("Scrobbling: allow band3 to scrobble to your Last.fm account at {}", url);
        if (!SDL_OpenURL(url.c_str())) {
            REXLOG_WARN("Scrobbling: couldn't open the browser; open the address above yourself");
        }
    }

    void PollLastfmAuth(Clock::time_point now) {
        const web::Reply reply = CallLastfm(LastfmGetSession(kLastfmApiKey, token_));
        if (Stopping()) return;
        std::string message;
        const Outcome outcome = LastfmOutcome(reply.status, reply.body, &message);
        const std::string session =
            outcome == Outcome::kOk ? LastfmSessionKey(reply.body) : std::string();
        if (!session.empty()) {
            authorizing_ = false;
            token_.clear();
            SaveSession(session);
            REXLOG_INFO("Scrobbling: signed in to Last.fm");
            return;
        }
        if ((outcome == Outcome::kNotAuthorized || outcome == Outcome::kRetry) &&
            now < auth_deadline_) {
            next_auth_poll_ = now + kAuthPoll;
            return;
        }
        authorizing_ = false;
        lastfm_.on = false;
        lastfm_.pending.clear();
        REXLOG_WARN("Scrobbling: Last.fm sign-in {}; Last.fm is off until the next start",
                    outcome == Outcome::kNotAuthorized ? "wasn't allowed in time"
                                                       : "failed (" + Describe(reply, message) + ")");
    }

    void NowPlaying(const Track& track) {
        if (lastfm_.on && !authorizing_) {
            const web::Reply reply =
                CallLastfm(LastfmNowPlaying(track, kLastfmApiKey, config_.lastfm_session));
            if (Stopping()) return;
            std::string message;
            const Outcome outcome = LastfmOutcome(reply.status, reply.body, &message);
            if (outcome == Outcome::kBadCredentials) DropLastfmSession(message);
            else if (outcome != Outcome::kOk) Warn(lastfm_warned_, "Last.fm", reply, message);
        }
        if (listenbrainz_.on) {
            const Track one[] = {track};
            const web::Reply reply = PostListenBrainz(ListenBrainzJson("playing_now", one));
            if (Stopping()) return;
            const Outcome outcome = ListenBrainzOutcome(reply.status);
            if (outcome == Outcome::kBadCredentials) DropListenBrainz();
            else if (outcome != Outcome::kOk) Warn(listenbrainz_warned_, "ListenBrainz", reply);
        }
    }

    void SendLastfm(Clock::time_point now) {
        const std::vector<Track> batch(
            lastfm_.pending.begin(),
            lastfm_.pending.begin() + std::min(lastfm_.pending.size(), kLastfmBatch));
        const web::Reply reply =
            CallLastfm(LastfmScrobble(batch, kLastfmApiKey, config_.lastfm_session));
        if (Stopping()) return;
        std::string message;
        switch (LastfmOutcome(reply.status, reply.body, &message)) {
        case Outcome::kOk:
            REXLOG_INFO("Scrobbling: scrobbled {} to Last.fm", Count(batch.size()));
            lastfm_.Sent(batch.size());
            lastfm_warned_ = false;
            break;
        case Outcome::kBadCredentials: DropLastfmSession(message); break;
        case Outcome::kRejected:
            REXLOG_WARN("Scrobbling: Last.fm refused {} ({}); dropped", Count(batch.size()),
                        Describe(reply, message));
            lastfm_.Sent(batch.size());
            break;
        default:
            Warn(lastfm_warned_, "Last.fm", reply, message);
            lastfm_.Failed(now);
            break;
        }
    }

    void SendListenBrainz(Clock::time_point now) {
        const std::vector<Track> batch(
            listenbrainz_.pending.begin(),
            listenbrainz_.pending.begin() + std::min(listenbrainz_.pending.size(), kListenBrainzBatch));
        const web::Reply reply =
            PostListenBrainz(ListenBrainzJson(batch.size() == 1 ? "single" : "import", batch));
        if (Stopping()) return;
        switch (ListenBrainzOutcome(reply.status)) {
        case Outcome::kOk:
            REXLOG_INFO("Scrobbling: submitted {} to ListenBrainz", Count(batch.size()));
            listenbrainz_.Sent(batch.size());
            listenbrainz_warned_ = false;
            break;
        case Outcome::kBadCredentials: DropListenBrainz(); break;
        case Outcome::kRejected:
            REXLOG_WARN("Scrobbling: ListenBrainz refused {} ({}); dropped", Count(batch.size()),
                        Describe(reply));
            listenbrainz_.Sent(batch.size());
            break;
        default:
            Warn(listenbrainz_warned_, "ListenBrainz", reply);
            listenbrainz_.Failed(now);
            break;
        }
    }

    web::Reply PostListenBrainz(const std::string& json) {
        return web::PostJson(config_.listenbrainz_url + "/1/submit-listens", json,
                             kListenBrainzTimeoutMs, "Token " + config_.listenbrainz_token);
    }

    void DropListenBrainz() {
        REXLOG_WARN("Scrobbling: ListenBrainz refused the user token; check listenbrainz_token "
                    "against listenbrainz.org/settings. ListenBrainz is off until the next start");
        listenbrainz_.on = false;
        listenbrainz_.pending.clear();
    }

    // one warning until the service answers again
    static void Warn(bool& warned, std::string_view service, const web::Reply& reply,
                     const std::string& message = {}) {
        if (warned) return;
        warned = true;
        REXLOG_WARN("Scrobbling: couldn't reach {} ({}); trying again later", service,
                    Describe(reply, message));
    }

    static std::string Count(size_t n) {
        return n == 1 ? std::string("1 song") : std::to_string(n) + " songs";
    }

    std::shared_ptr<State> state_;
    Config config_;
    Queue lastfm_;
    Queue listenbrainz_;
    bool lastfm_warned_ = false;
    bool listenbrainz_warned_ = false;
    // signing in to Last.fm: the token the player is allowing
    bool authorizing_ = false;
    std::string token_;
    Clock::time_point next_auth_poll_{};
    Clock::time_point auth_deadline_{};
};

int64_t UnixNow() { return static_cast<int64_t>(std::time(nullptr)); }

// the song on now has ended: a listen, if it was played long enough. Called
// with the state's mutex held.
void EndPlaying(State& state) {
    if (!state.playing) return;
    const Track& track = *state.playing;
    if (ShouldScrobble(track.length_ms, UnixNow() - track.started)) {
        state.jobs.push_back({true, track});
    }
    state.playing.reset();
}

}

void Start(const std::filesystem::path& config_path) {
    std::lock_guard control(g_control);
    const auto& s = band3::settings::Startup();
    if (g_started) return;

    Config config;
    config.config_path = config_path;
    config.lastfm_url = s.lastfm_api_url;
    config.listenbrainz_url = s.listenbrainz_api_url;
    while (!config.listenbrainz_url.empty() && config.listenbrainz_url.back() == '/') {
        config.listenbrainz_url.pop_back();
    }
    if (s.lastfm_enabled) {
        if (kLastfmApiKey.empty() || kLastfmApiSecret.empty()) {
            REXLOG_WARN("Scrobbling: this build has no Last.fm API key "
                        "(BAND3_LASTFM_API_KEY), so it can't scrobble to Last.fm");
        } else {
            config.lastfm = true;
            config.lastfm_session = s.lastfm_session;
        }
    }
    if (s.listenbrainz_enabled) {
        if (s.listenbrainz_token.empty()) {
            REXLOG_WARN("Scrobbling: ListenBrainz needs your user token (listenbrainz_token), "
                        "from listenbrainz.org/settings");
        } else {
            config.listenbrainz = true;
            config.listenbrainz_token = s.listenbrainz_token;
        }
    }
    if (!config.lastfm && !config.listenbrainz) return;

    REXLOG_INFO("Scrobbling: to {}{}{}", config.lastfm ? "Last.fm" : "",
                config.lastfm && config.listenbrainz ? " and " : "",
                config.listenbrainz ? "ListenBrainz" : "");
    g_state = std::make_shared<State>();
    g_thread = std::thread([state = g_state, config = std::move(config)]() mutable {
        Worker(std::move(state), std::move(config)).Run();
    });
    g_started = true;
}

void Stop() {
    std::lock_guard control(g_control);
    if (!g_started) return;
    g_started = false;
    std::shared_ptr<State> state = std::move(g_state);
    bool done;
    {
        std::unique_lock lock(state->mutex);
        state->stop = true;
        state->cv.notify_all();
        done = state->cv.wait_for(lock, kStopWait, [&] { return state->done; });
    }
    // a request that outlasts the wait isn't worth holding band3's exit for;
    // the thread keeps its own hold on the state
    if (done) g_thread.join();
    else g_thread.detach();
}

bool Enabled() { return g_started.load(); }

void SongStarted(const std::string& title, const std::string& artist, int32_t length_ms) {
    if (!Enabled()) return;
    std::shared_ptr<State> state;
    {
        std::lock_guard control(g_control);
        state = g_state;
    }
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        EndPlaying(*state);
        Track track{http::ToUtf8(title), http::ToUtf8(artist), length_ms, UnixNow()};
        if (track.title.empty() || track.artist.empty()) return;
        state->playing = track;
        state->jobs.push_back({false, std::move(track)});
    }
    state->cv.notify_all();
}

void SongEnded() {
    if (!Enabled()) return;
    std::shared_ptr<State> state;
    {
        std::lock_guard control(g_control);
        state = g_state;
    }
    if (!state) return;
    {
        std::lock_guard lock(state->mutex);
        EndPlaying(*state);
    }
    state->cv.notify_all();
}

}
