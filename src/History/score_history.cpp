#include "score_history.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>
#include "src/Net/http_request.h"
#include "src/Net/json.h"

namespace band3::history {

namespace {

using http::JsonString;

constexpr std::string_view kParts[] = {"drum",      "bass",        "guitar",    "vocals",
                                       "harmony",   "keys",        "real_drum", "real_guitar",
                                       "real_bass", "real_keys"};

bool KnownPart(std::string_view part) {
    return std::ranges::find(kParts, part) != std::end(kParts);
}

void AppendField(std::string& out, std::string_view name, int64_t value) {
    out += ",\"";
    out += name;
    out += "\":";
    out += std::to_string(value);
}

// a run's fields after its leading '{', shared by the file and the endpoints
std::string RunFields(const Run& run) {
    std::string out = "\"v\":" + std::to_string(kVersion);
    AppendField(out, "t", run.time);
    out += ",\"shortname\":" + JsonString(run.shortname);
    AppendField(out, "song_id", run.song_id);
    out += ",\"title\":" + JsonString(run.title);
    out += ",\"artist\":" + JsonString(run.artist);
    out += ",\"part\":" + JsonString(run.part);
    AppendField(out, "diff", run.diff);
    AppendField(out, "score", run.score);
    AppendField(out, "stars", run.stars);
    AppendField(out, "pct", run.pct);
    AppendField(out, "streak", run.streak);
    AppendField(out, "od_deploys", run.od_deploys);
    if (run.solo_pct) AppendField(out, "solo_pct", *run.solo_pct);
    if (run.unison) AppendField(out, "unison", *run.unison);
    AppendField(out, "band_size", run.band_size);
    AppendField(out, "speed", run.speed);
    out += ",\"modifiers\":[";
    for (size_t i = 0; i < run.modifiers.size(); i++) {
        if (i) out += ',';
        out += JsonString(run.modifiers[i]);
    }
    out += ']';
    if (run.autoplay) out += ",\"autoplay\":true";
    return out;
}

std::string RunJson(const Run& run) {
    return "{" + RunFields(run) + ",\"counts\":" + (run.Counts() ? "true" : "false") + "}";
}

std::optional<int> OptionalInt(const json::Value& v) {
    if (!v.IsNumber()) return std::nullopt;
    return static_cast<int>(v.Number());
}

}  // namespace

bool Run::Counts() const {
    return modifiers.empty() && speed == 100 && !autoplay;
}

std::string FormatLine(const Run& run) {
    return "{" + RunFields(run) + "}";
}

std::optional<Run> ParseLine(std::string_view line) {
    const auto v = json::Parse(line);
    if (!v || !v->IsObject()) return std::nullopt;
    const json::Value& o = *v;
    if (!o["v"].IsNumber() || o["v"].Number() > kVersion) return std::nullopt;
    if (!o["shortname"].IsString() || !o["part"].IsString() || !o["score"].IsNumber()) {
        return std::nullopt;
    }
    Run run;
    run.time = static_cast<int64_t>(o["t"].Number());
    run.shortname = o["shortname"].Text();
    run.song_id = static_cast<int32_t>(o["song_id"].Number());
    run.title = o["title"].Text();
    run.artist = o["artist"].Text();
    run.part = o["part"].Text();
    run.diff = static_cast<int>(o["diff"].Number());
    run.score = static_cast<int>(o["score"].Number());
    run.stars = static_cast<int>(o["stars"].Number());
    run.pct = static_cast<int>(o["pct"].Number());
    run.streak = static_cast<int>(o["streak"].Number());
    run.od_deploys = static_cast<int>(o["od_deploys"].Number());
    run.solo_pct = OptionalInt(o["solo_pct"]);
    run.unison = OptionalInt(o["unison"]);
    run.band_size = static_cast<int>(o["band_size"].Number(1));
    run.speed = static_cast<int>(o["speed"].Number(100));
    for (const auto& m : o["modifiers"].Items()) run.modifiers.push_back(m.Text());
    run.autoplay = o["autoplay"].Bool();
    if (run.shortname.empty()) return std::nullopt;
    return run;
}

bool Plausible(const Run& run) {
    auto in = [](int v, int lo, int hi) { return v >= lo && v <= hi; };
    return !run.shortname.empty() && KnownPart(run.part) && in(run.diff, 0, 3) &&
           run.score >= 0 && in(run.stars, 0, 6) && in(run.pct, 0, 100) && run.streak >= 0 &&
           run.od_deploys >= 0 && (!run.solo_pct || in(*run.solo_pct, 0, 100)) &&
           (!run.unison || *run.unison >= 0) && in(run.band_size, 1, 4) && in(run.speed, 1, 1000);
}

std::string PartKey(std::string_view part, int diff) {
    return std::string(part) + ":" + std::to_string(diff);
}

void Index::Add(Run run) {
    SongSummary& song = songs_[run.shortname];
    song.plays++;
    song.last = std::max(song.last, run.time);
    PartSummary& part = song.parts[PartKey(run.part, run.diff)];
    part.plays++;
    part.last = std::max(part.last, run.time);
    if (run.Counts()) {
        part.best_score = std::max(part.best_score, run.score);
        part.best_stars = std::max(part.best_stars, run.stars);
        part.best_pct = std::max(part.best_pct, run.pct);
    }
    runs_.push_back(std::move(run));
}

Index ParseFile(std::string_view text) {
    Index index;
    while (!text.empty()) {
        const size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        if (line.empty()) continue;
        if (auto run = ParseLine(line)) {
            index.Add(std::move(*run));
        } else {
            index.skipped++;
        }
    }
    return index;
}

std::string SummaryJson(const Index& index) {
    std::string out = "{\"enabled\":true,\"songs\":{";
    bool first_song = true;
    for (const auto& [shortname, song] : index.Songs()) {
        if (!first_song) out += ',';
        first_song = false;
        out += JsonString(shortname) + ":{\"plays\":" + std::to_string(song.plays) +
               ",\"last\":" + std::to_string(song.last) + ",\"parts\":{";
        bool first_part = true;
        for (const auto& [key, part] : song.parts) {
            if (!first_part) out += ',';
            first_part = false;
            out += JsonString(key) + ":{\"plays\":" + std::to_string(part.plays) +
                   ",\"last\":" + std::to_string(part.last) +
                   ",\"best_score\":" + std::to_string(part.best_score) +
                   ",\"best_stars\":" + std::to_string(part.best_stars) +
                   ",\"best_pct\":" + std::to_string(part.best_pct) + "}";
        }
        out += "}}";
    }
    return out + "}}";
}

std::string RunsJson(const Index& index, std::string_view shortname, std::string_view part,
                     int diff) {
    std::string out = "{\"enabled\":true,\"runs\":[";
    bool first = true;
    const auto& runs = index.Runs();
    for (auto it = runs.rbegin(); it != runs.rend(); ++it) {
        if (it->shortname != shortname) continue;
        if (!part.empty() && it->part != part) continue;
        if (diff >= 0 && it->diff != diff) continue;
        if (!first) out += ',';
        first = false;
        out += RunJson(*it);
    }
    return out + "]}";
}

std::string RecentJson(const Index& index, size_t limit) {
    std::string out = "{\"enabled\":true,\"runs\":[";
    const auto& runs = index.Runs();
    size_t n = 0;
    for (auto it = runs.rbegin(); it != runs.rend() && n < limit; ++it, ++n) {
        if (n) out += ',';
        out += RunJson(*it);
    }
    return out + "]}";
}

std::string DisabledJson() {
    return "{\"enabled\":false}";
}

struct Store::Impl {
    mutable std::mutex mutex;  // the index
    Index index;

    std::mutex queue_mutex;
    std::condition_variable wake;
    std::condition_variable drained;
    std::deque<std::string> lines;
    bool writing = false;
    bool stop = false;
    std::thread writer;
};

Store::Store(std::filesystem::path path) : path_(std::move(path)), impl_(new Impl) {
    std::ifstream in(path_, std::ios::binary);
    if (in) {
        std::ostringstream text;
        text << in.rdbuf();
        impl_->index = ParseFile(text.str());
    }
    impl_->writer = std::thread([this] {
        Impl& d = *impl_;
        std::unique_lock lock(d.queue_mutex);
        for (;;) {
            d.wake.wait(lock, [&] { return d.stop || !d.lines.empty(); });
            if (d.lines.empty()) break;  // stopping, all written
            std::deque<std::string> batch;
            batch.swap(d.lines);
            d.writing = true;
            lock.unlock();
            std::error_code ec;
            if (path_.has_parent_path()) std::filesystem::create_directories(path_.parent_path(), ec);
            // binary, so the lines end in \n on every platform
            if (std::ofstream out{path_, std::ios::binary | std::ios::app}) {
                for (const auto& line : batch) out << line << '\n';
            }
            lock.lock();
            d.writing = false;
            d.drained.notify_all();
        }
    });
}

Store::~Store() {
    {
        std::lock_guard lock(impl_->queue_mutex);
        impl_->stop = true;
    }
    impl_->wake.notify_all();
    impl_->writer.join();
    delete impl_;
}

void Store::Append(Run run) {
    std::string line = FormatLine(run);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->index.Add(std::move(run));
    }
    {
        std::lock_guard lock(impl_->queue_mutex);
        impl_->lines.push_back(std::move(line));
    }
    impl_->wake.notify_all();
}

void Store::Flush() {
    std::unique_lock lock(impl_->queue_mutex);
    impl_->drained.wait(lock, [&] { return impl_->lines.empty() && !impl_->writing; });
}

std::string Store::Summary() const {
    std::lock_guard lock(impl_->mutex);
    return SummaryJson(impl_->index);
}

std::string Store::Runs(std::string_view shortname, std::string_view part, int diff) const {
    std::lock_guard lock(impl_->mutex);
    return RunsJson(impl_->index, shortname, part, diff);
}

std::string Store::Recent(size_t limit) const {
    std::lock_guard lock(impl_->mutex);
    return RecentJson(impl_->index, limit);
}

namespace {
// opened once and kept for the process's life: the hook and the web server
// read it from their own threads
std::atomic<Store*> g_store{nullptr};
std::once_flag g_open;
}

void Open(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::call_once(g_open, [&] { g_store.store(new Store(path)); });
}

Store* Get() {
    return g_store.load();
}

}
