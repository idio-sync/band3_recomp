#include "home_assistant.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>
#include <rex/logging.h>
#include "src/build_tag.h"
#include "src/settings.h"
#include "src/Test/game_state.h"
#include "discord.h"
#include "events.h"
#include "ha_entities.h"
#include "mqtt_client.h"
#include "web_client.h"

namespace band3::ha {

namespace {

using Clock = std::chrono::steady_clock;

// how often the webhook's watcher looks at the game state, as the MQTT
// client's produce is called
constexpr std::chrono::milliseconds kWatchTick{50};
// a POST's whole budget: a trigger that comes late is worse than none
constexpr int kPostTimeoutMs = 2000;
// payloads waiting to be POSTed; past this the oldest go, so a Home Assistant
// that stopped answering can't pile them up
constexpr size_t kQueueLimit = 16;
// at most one "queue full" warning this often
constexpr std::chrono::seconds kDropWarnInterval{30};
// how long Stop waits for a POST in flight before it leaves it be
constexpr std::chrono::milliseconds kPosterStopWait{2500};

// The webhook's threads share this. They hold it themselves, so a poster left
// behind in a slow POST (see Stop) still has it after Stop lets go.
struct Webhook {
    std::string url;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> queue;
    bool stop = false;
    bool poster_done = false;
    // dropped since the last warning, and when that warning went
    int dropped = 0;
    std::optional<Clock::time_point> warned;
};

// Start and Stop, one at a time
std::mutex g_control;
bool g_started = false;
std::atomic<bool> g_configured{false};
std::atomic<bool> g_mqtt{false};
// whether NoteStageKit keeps the lights, and the lights (PackStageKit's word)
std::atomic<bool> g_stagekit_on{false};
std::atomic<uint64_t> g_stagekit{0};
Options g_options;
std::shared_ptr<Webhook> g_webhook;
std::thread g_watcher;
std::thread g_poster;

// never destroyed: its thread may still be running when the process exits
// without OnShutdown (closing the window), and must not meet a destructor
mqtt::Client& Client() {
    static auto* client = new mqtt::Client;
    return *client;
}

// the PC's name as the system has it, UTF-8
std::string PcName() {
#ifdef _WIN32
    wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
    DWORD length = static_cast<DWORD>(std::size(name));
    if (!GetComputerNameW(name, &length) || length == 0) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), nullptr, 0,
                                          nullptr, nullptr);
    if (bytes <= 0) return {};
    std::string out(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, name, static_cast<int>(length), out.data(), bytes, nullptr,
                        nullptr);
    return out;
#else
    char name[256] = {};
    if (gethostname(name, sizeof(name) - 1) != 0) return {};
    return name;
#endif
}

// what the hooks have kept, as HA is told it
GameView CurrentView() {
    const test::GameStateSnapshot game = test::GameState::Get().Snapshot();
    GameView view;
    view.screen = discord::detail::ToUtf8(game.screen);
    view.in_game = game.in_game;
    view.paused = game.paused;
    view.title = discord::detail::ToUtf8(game.song_name);
    view.artist = discord::detail::ToUtf8(game.song_artist);
    view.shortname = discord::detail::ToUtf8(game.song_shortname);
    view.length_ms = game.song_length_ms;
    view.song_ms = game.song_ms;
    view.venue = discord::detail::ToUtf8(game.venue);
    view.score = game.score;
    events::BandInfo band{};
    for (size_t i = 0; i < game.band.size(); i++) {
        band.member_exists[i] = game.band[i].exists ? 1 : 0;
        band.difficulty[i] = game.band[i].difficulty;
        band.track_type[i] = game.band[i].track_type;
    }
    view.band = discord::detail::BandText(band);
    return view;
}

// the client's lines: its comings and goings are news, the rest are failures
void LogMqtt(const std::string& line) {
    if (line.starts_with("connecting to ") || line.starts_with("connected to ")) {
        REXLOG_INFO("ha: {}", line);
    } else {
        REXLOG_WARN("ha: {}", line);
    }
}

// the URL as the log shows it, without a query string
std::string_view WithoutQuery(std::string_view url) {
    return url.substr(0, url.find('?'));
}

// the watcher's thread: the dashboard's payloads, from the game state as it changes
void Watch(std::shared_ptr<Webhook> hook) {
    WebhookWatcher watcher;
    std::unique_lock lock(hook->mutex);
    while (!hook->stop) {
        lock.unlock();
        std::vector<std::string> payloads = watcher.Changes(CurrentView());
        lock.lock();
        if (!payloads.empty()) {
            int dropped = 0;
            for (std::string& payload : payloads) {
                if (hook->queue.size() >= kQueueLimit) {
                    hook->queue.pop_front();
                    hook->dropped++;
                }
                hook->queue.push_back(std::move(payload));
            }
            const Clock::time_point now = Clock::now();
            if (hook->dropped > 0 &&
                (!hook->warned || now - *hook->warned >= kDropWarnInterval)) {
                dropped = std::exchange(hook->dropped, 0);
                hook->warned = now;
            }
            hook->cv.notify_all();
            if (dropped > 0) {
                REXLOG_WARN("ha: webhook queue full, dropped {} payload{}", dropped,
                            dropped == 1 ? "" : "s");
            }
        }
        hook->cv.wait_for(lock, kWatchTick, [&] { return hook->stop; });
    }
}

// the poster's thread: one POST at a time, no retries
void Post(std::shared_ptr<Webhook> hook) {
    // the failure logged last, so a Home Assistant that's down is said once
    std::string last_error;
    std::unique_lock lock(hook->mutex);
    while (true) {
        hook->cv.wait(lock, [&] { return hook->stop || !hook->queue.empty(); });
        if (hook->stop) break;
        const std::string payload = std::move(hook->queue.front());
        hook->queue.pop_front();
        lock.unlock();
        const web::Reply reply = web::PostJson(hook->url, payload, kPostTimeoutMs);
        std::string error;
        if (reply.status == 0) {
            error = reply.error.empty() ? "no reply" : reply.error;
        } else if (reply.status < 200 || reply.status > 299) {
            error = "HTTP " + std::to_string(reply.status);
        }
        lock.lock();
        // past Stop the log may be gone
        if (hook->stop) break;
        if (error.empty()) {
            last_error.clear();
        } else if (error != last_error) {
            last_error = error;
            REXLOG_WARN("ha: webhook: {}", error);
        }
    }
    hook->poster_done = true;
    hook->cv.notify_all();
}

void StopWebhook() {
    if (!g_webhook) return;
    std::shared_ptr<Webhook> hook = std::move(g_webhook);
    {
        std::lock_guard lock(hook->mutex);
        hook->stop = true;
    }
    hook->cv.notify_all();
    if (g_watcher.joinable()) g_watcher.join();
    bool done;
    {
        std::unique_lock lock(hook->mutex);
        done = hook->cv.wait_for(lock, kPosterStopWait, [&] { return hook->poster_done; });
    }
    if (!g_poster.joinable()) return;
    // a POST that outlasts its timeout (WinHTTP's applies to each step) isn't
    // worth holding band3's exit for; the thread keeps its own hold on `hook`
    if (done) {
        g_poster.join();
    } else {
        g_poster.detach();
    }
}

}  // namespace

void Start() {
    std::lock_guard control(g_control);
    if (g_started) return;
    const settings::StartupSettings& s = settings::Startup();
    const bool mqtt = !s.ha_mqtt_host.empty();
    const bool webhook = !s.ha_webhook_url.empty();
    if (!mqtt && !webhook) return;
    g_started = true;
    g_configured = true;

    const std::string pc_name = PcName();
    // a prefix cleared by mistake would put the configs where HA never looks
    const std::string prefix =
        s.ha_discovery_prefix.empty() ? std::string("homeassistant") : s.ha_discovery_prefix;
    g_options = Options{PcId(pc_name), pc_name, prefix, s.ha_stagekit, BuildTag()};

    if (mqtt) {
        g_stagekit.store(PackStageKit({}), std::memory_order_relaxed);
        g_stagekit_on = s.ha_stagekit;
        mqtt::ClientConfig config;
        config.host = s.ha_mqtt_host;
        config.port = static_cast<uint16_t>(std::clamp(s.ha_mqtt_port, 1, 65535));
        config.connect.client_id = "band3_" + g_options.pc_id;
        config.connect.username = s.ha_mqtt_username;
        config.connect.password = s.ha_mqtt_password;
        config.connect.will = mqtt::Will{StatusTopic(g_options), "offline", true};
        config.connect.keepalive_s = 60;
        config.log = LogMqtt;
        // the client's thread is the only one that touches it
        auto publisher = std::make_shared<Publisher>(g_options);
        Client().Start(std::move(config), [publisher](bool fresh) {
            const StageKit stagekit =
                UnpackStageKit(g_stagekit.load(std::memory_order_relaxed));
            return publisher->Changes(CurrentView(), stagekit, Clock::now(), fresh);
        });
        g_mqtt = true;
        REXLOG_INFO("ha: MQTT as band3/{}{}", g_options.pc_id,
                    s.ha_stagekit ? ", with the Stage Kit" : "");
    }

    if (webhook) {
        auto hook = std::make_shared<Webhook>();
        hook->url = s.ha_webhook_url;
        g_webhook = hook;
        g_watcher = std::thread(Watch, hook);
        g_poster = std::thread(Post, hook);
        REXLOG_INFO("ha: webhook to {}", WithoutQuery(hook->url));
    }
}

void Stop() {
    std::lock_guard control(g_control);
    if (!g_started) return;
    g_started = false;
    g_stagekit_on = false;
    StopWebhook();
    if (g_mqtt) Client().Stop({mqtt::Message{StatusTopic(g_options), "offline", true}});
}

bool Configured() {
    return g_configured.load(std::memory_order_relaxed);
}

std::string StateName() {
    if (!g_mqtt.load()) return "off";
    return mqtt::StatusText(Client().GetStatus());
}

void NoteStageKit(uint8_t left, uint8_t right) {
    if (!g_stagekit_on.load(std::memory_order_relaxed)) return;
    uint64_t old = g_stagekit.load(std::memory_order_relaxed);
    while (!g_stagekit.compare_exchange_weak(
        old, PackStageKit(ApplyStageKit(UnpackStageKit(old), left, right)),
        std::memory_order_relaxed)) {
    }
}

}  // namespace band3::ha
