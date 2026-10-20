#include "hotspot.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "hotspot_plan.h"
#include "src/Video/child_process.h"
#include "src/settings.h"
#include "src/steam_deck.h"

namespace band3::lights::hotspot {

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

// a change waits this long for the next before it's applied, so a name typed
// a letter at a time starts the hotspot once
constexpr auto kSettle = 1s;
// how often a running hotspot is checked on
constexpr auto kCheckInterval = 5s;

struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    bool stop = false;
    // what the settings ask for (Follow), from the UI thread: nullopt is off
    std::optional<Config> wanted;
    Clock::time_point wanted_since{};
    Status status;
    std::thread thread;
    // ends an nmcli that's running, at Stop
    std::atomic<bool> cancel{false};
};
Shared g;
std::mutex g_lifecycle_mutex;
bool g_started = false;

struct Ran {
    bool ok = false;
    int exit_code = -1;
    std::vector<std::string> lines;
    // nmcli itself couldn't be run
    bool missing = false;
};

Ran Nmcli(const std::vector<std::string>& args) {
    Ran ran;
    const video::ProcessResult result = video::RunProcess(
        "nmcli", args, [&](std::string_view line) { ran.lines.emplace_back(line); }, &g.cancel);
    ran.missing = !result.started;
    ran.exit_code = result.exit_code;
    ran.ok = result.started && !result.cancelled && result.exit_code == 0;
    if (ran.missing) REXLOG_WARN("Hotspot: nmcli couldn't be run: {}", result.error);
    return ran;
}

void SetStatus(Status status) {
    std::lock_guard<std::mutex> lock(g.mutex);
    g.status = std::move(status);
}

Status Problem(std::string text) {
    REXLOG_WARN("Hotspot: {}", text);
    return {.state = Status::State::kProblem, .problem = std::move(text)};
}

bool HasDnsmasq() {
    std::error_code error;
    for (const std::string_view path : kDnsmasqPaths) {
        if (std::filesystem::exists(path, error)) return true;
    }
    return false;
}

constexpr std::string_view kNoNmcli =
    "This system has no NetworkManager (nmcli), so band3 can't run a hotspot.";

class Runner {
public:
    void Run() {
        std::optional<Config> applied;
        bool acted = false;
        for (;;) {
            std::optional<Config> wanted;
            Clock::time_point since;
            {
                std::unique_lock<std::mutex> lock(g.mutex);
                g.cv.wait_for(lock, 250ms, [] { return g.stop; });
                if (g.stop) break;
                wanted = g.wanted;
                since = g.wanted_since;
            }
            const auto now = Clock::now();
            // the first time at once: a hotspot a band3 that crashed left up
            // goes down with the setting off
            if (!acted || (wanted != applied && now - since >= kSettle)) {
                Apply(wanted, !acted);
                applied = wanted;
                acted = true;
            }
            if (up_ && now >= next_check_) Check(now);
        }
        g.cancel = false;
        if (maybe_up_) Down();
    }

private:
    void Apply(const std::optional<Config>& wanted, bool first) {
        if (!wanted) {
            if (maybe_up_ || (first && Active())) Down();
            SetStatus({});
            return;
        }
        if (const std::string problem = ConfigProblem(*wanted); !problem.empty()) {
            if (maybe_up_) Down();
            SetStatus({.state = Status::State::kProblem, .problem = problem});
            return;
        }
        SetStatus({.state = Status::State::kStarting, .name = wanted->name});
        const Ran devices = Nmcli(DevicesArgs());
        if (devices.missing) return SetStatus(Problem(std::string(kNoNmcli)));
        const auto device = FindWifiDevice(devices.lines);
        if (!device) return SetStatus(Problem("This Deck's Wi-Fi wasn't found."));
        if (!device->available) {
            return SetStatus(Problem("The Wi-Fi is off. Turn it on (airplane mode off) for the hotspot."));
        }
        const Addressing addressing = HasDnsmasq() ? Addressing::kDhcp : Addressing::kLinkLocal;
        Ran made = Nmcli(ModifyArgs(*wanted, addressing));
        if (!made.ok && made.exit_code == kNotFound) {
            made = Nmcli(AddArgs(*wanted, device->name, addressing));
        }
        if (!made.ok) {
            return SetStatus(Problem("NetworkManager wouldn't make the hotspot: " + ErrorText(made.lines)));
        }
        REXLOG_INFO("Hotspot: starting {} on {}", wanted->name, device->name);
        maybe_up_ = true;
        const Ran started = Nmcli(UpArgs(device->name));
        if (!started.ok) {
            if (g.cancel) return;
            up_ = false;
            return SetStatus(Problem("The hotspot didn't start: " + ErrorText(started.lines)));
        }
        const std::string address = FirstAddress(Nmcli(AddressArgs(device->name)).lines);
        REXLOG_INFO("Hotspot: {} is up, this Deck at {} ({})", wanted->name, address,
                    addressing == Addressing::kDhcp ? "DHCP" : "link-local, no dnsmasq");
        up_ = true;
        next_check_ = Clock::now() + kCheckInterval;
        SetStatus({.state = Status::State::kOn,
                   .name = wanted->name,
                   .address = address,
                   .link_local = addressing == Addressing::kLinkLocal});
    }

    // a running hotspot that stopped (Wi-Fi joined another network, or turned
    // off) isn't started again until the settings change: that could take the
    // Wi-Fi back from a network the player picked
    void Check(Clock::time_point now) {
        next_check_ = now + kCheckInterval;
        const Ran active = Nmcli(ActiveArgs());
        if (!active.ok || ListsConnection(active.lines)) return;
        up_ = false;
        maybe_up_ = false;
        SetStatus(Problem("The hotspot stopped: the Wi-Fi joined another network, or was turned off. "
                          "Turn Wi-Fi hotspot off, then on, to start it again."));
    }

    bool Active() {
        const Ran active = Nmcli(ActiveArgs());
        return active.ok && ListsConnection(active.lines);
    }

    void Down() {
        REXLOG_INFO("Hotspot: stopping it");
        Nmcli(DownArgs());
        up_ = false;
        maybe_up_ = false;
    }

    // band3 started the hotspot and it's running
    bool up_ = false;
    // band3 asked for it to start, so it may be running: Stop takes it down
    bool maybe_up_ = false;
    Clock::time_point next_check_{};
};

}

void Follow() {
    std::optional<Config> wanted;
    if (REXCVAR_GET(wifi_hotspot)) {
        wanted = Config{REXCVAR_GET(wifi_hotspot_name), REXCVAR_GET(wifi_hotspot_password)};
    }
    std::lock_guard<std::mutex> lock(g.mutex);
    if (wanted == g.wanted) return;
    g.wanted = std::move(wanted);
    g.wanted_since = Clock::now();
}

void Start() {
#ifndef _WIN32
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (g_started) return;
    // a test harness run leaves the Deck's Wi-Fi alone
    if (!steam_deck::IsSteamDeck() || REXCVAR_GET(test_port) != 0) return;
    g_started = true;
    Follow();
    {
        std::lock_guard<std::mutex> shared(g.mutex);
        g.stop = false;
    }
    g.cancel = false;
    g.thread = std::thread([] { Runner().Run(); });
#endif
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_lifecycle_mutex);
    if (!g_started) return;
    {
        std::lock_guard<std::mutex> shared(g.mutex);
        g.stop = true;
    }
    g.cancel = true;
    g.cv.notify_one();
    if (g.thread.joinable()) g.thread.join();
    SetStatus({});
    g_started = false;
}

Status Current() {
    std::lock_guard<std::mutex> lock(g.mutex);
    return g.status;
}

}
