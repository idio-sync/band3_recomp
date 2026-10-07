#include "ha_entities.h"

#include <algorithm>
#include <bit>
#include "http_request.h"

namespace band3::ha {

namespace {

// A JSON object written key by key, in the order given, so the configs come
// out the same every time and tests compare them whole.
class JsonObject {
public:
    JsonObject& Raw(std::string_view key, std::string_view value) {
        if (text_.size() > 1) text_ += ',';
        text_ += '"';
        text_ += key;
        text_ += "\":";
        text_ += value;
        return *this;
    }
    JsonObject& Text(std::string_view key, std::string_view value) {
        return Raw(key, http::JsonString(value));
    }
    JsonObject& Number(std::string_view key, int64_t value) {
        return Raw(key, std::to_string(value));
    }
    std::string Close() const { return text_ + "}"; }

private:
    std::string text_ = "{";
};

// One of the entities HA is told about. The extras are left out when null.
struct Entity {
    const char* key;
    const char* component;  // "sensor" or "binary_sensor"
    const char* name;
    bool attributes;  // has a json_attributes_topic
    const char* device_class;
    const char* state_class;
    const char* unit;
    const char* icon;
};

constexpr Entity kGameEntities[] = {
    {"song", "sensor", "Song", true, nullptr, nullptr, nullptr, "mdi:music-note"},
    {"artist", "sensor", "Artist", false, nullptr, nullptr, nullptr, "mdi:account-music"},
    {"venue", "sensor", "Venue", false, nullptr, nullptr, nullptr, "mdi:stadium"},
    {"score", "sensor", "Score", false, nullptr, "measurement", nullptr, "mdi:counter"},
    {"playing", "binary_sensor", "Playing", false, "running", nullptr, nullptr, nullptr},
    {"paused", "binary_sensor", "Paused", false, nullptr, nullptr, nullptr, "mdi:pause"},
    {"screen", "sensor", "Screen", false, nullptr, nullptr, nullptr, "mdi:monitor"},
    {"progress", "sensor", "Song progress", true, nullptr, "measurement", "%", nullptr},
    {"band", "sensor", "Band", false, nullptr, nullptr, nullptr, "mdi:account-group"},
};

constexpr Entity kStageKitEntities[] = {
    {"stagekit_red", "sensor", "Stage Kit red", true, nullptr, nullptr, nullptr, nullptr},
    {"stagekit_yellow", "sensor", "Stage Kit yellow", true, nullptr, nullptr, nullptr, nullptr},
    {"stagekit_green", "sensor", "Stage Kit green", true, nullptr, nullptr, nullptr, nullptr},
    {"stagekit_blue", "sensor", "Stage Kit blue", true, nullptr, nullptr, nullptr, nullptr},
    {"stagekit_strobe", "sensor", "Stage Kit strobe", false, nullptr, nullptr, nullptr, nullptr},
    {"stagekit_fog", "binary_sensor", "Stage Kit fog", false, nullptr, nullptr, nullptr, nullptr},
};

std::string Id(const Options& options) {
    return options.pc_id.empty() ? PcId(options.pc_name) : options.pc_id;
}

std::string StateTopic(const std::string& id, std::string_view entity) {
    return "band3/" + id + "/" + std::string(entity);
}

std::string AttributesTopic(const std::string& id, std::string_view entity) {
    return StateTopic(id, entity) + "/attributes";
}

std::string ConfigTopic(const Options& options, const std::string& id, const Entity& entity) {
    return options.discovery_prefix + "/" + entity.component + "/band3_" + id + "/" + entity.key +
           "/config";
}

mqtt::Message Config(const Options& options, const std::string& id, const Entity& entity) {
    const bool binary = std::string_view(entity.component) == "binary_sensor";
    JsonObject config;
    config.Text("name", entity.name)
        .Text("unique_id", "band3_" + id + "_" + entity.key)
        .Text("state_topic", StateTopic(id, entity.key));
    if (entity.attributes) config.Text("json_attributes_topic", AttributesTopic(id, entity.key));
    if (binary) config.Text("payload_on", "ON").Text("payload_off", "OFF");
    if (entity.device_class) config.Text("device_class", entity.device_class);
    if (entity.state_class) config.Text("state_class", entity.state_class);
    if (entity.unit) config.Text("unit_of_measurement", entity.unit);
    if (entity.icon) config.Text("icon", entity.icon);
    config.Text("availability_topic", StatusTopic(options))
        .Text("payload_available", "online")
        .Text("payload_not_available", "offline");

    JsonObject device;
    device.Raw("identifiers", "[" + http::JsonString("band3_" + id) + "]")
        .Text("name", "band3 (" + options.pc_name + ")")
        .Text("manufacturer", "band3")
        .Text("model", "Rock Band 3");
    if (!options.sw_version.empty()) device.Text("sw_version", options.sw_version);
    config.Raw("device", device.Close());

    return {ConfigTopic(options, id, entity), config.Close(), true};
}

std::string OnOff(bool on) { return on ? "ON" : "OFF"; }

// how a topic is held to its rate
enum class Rate { kAny, kProgress, kStageKit };

struct Entry {
    std::string topic;
    std::string payload;
    Rate rate = Rate::kAny;
};

}  // namespace

std::string PcId(std::string_view pc_name) {
    if (pc_name.empty()) return "pc";
    std::string id;
    id.reserve(pc_name.size());
    for (char c : pc_name) {
        if (c >= 'A' && c <= 'Z') {
            id += static_cast<char>(c - 'A' + 'a');
        } else if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) {
            id += c;
        } else {
            // a UTF-8 character's every byte, so "é" is two '_'
            id += '_';
        }
    }
    return id;
}

std::string StatusTopic(const Options& options) { return StateTopic(Id(options), "status"); }

std::vector<mqtt::Message> Discovery(const Options& options) {
    const std::string id = Id(options);
    std::vector<mqtt::Message> configs;
    for (const Entity& entity : kGameEntities) configs.push_back(Config(options, id, entity));
    if (options.stagekit) {
        for (const Entity& entity : kStageKitEntities) configs.push_back(Config(options, id, entity));
    }
    return configs;
}

Publisher::Publisher(Options options) : options_(std::move(options)) {
    progress_attributes_ = JsonObject().Number("position_ms", 0).Number("length_ms", 0).Close();
}

std::vector<mqtt::Message> Publisher::Changes(const GameView& view, const StageKit& stagekit,
                                              Clock::time_point now, bool fresh) {
    const std::string id = Id(options_);

    // progress is only worked out in a song, so it holds where the last one ended
    if (view.in_game) {
        int64_t percent = 0;
        if (view.song_ms >= 0 && view.length_ms > 0) {
            percent = std::clamp<int64_t>(int64_t(view.song_ms) * 100 / view.length_ms, 0, 100);
        }
        progress_ = std::to_string(percent);
        progress_attributes_ = JsonObject()
                                   .Number("position_ms", std::max<int32_t>(view.song_ms, 0))
                                   .Number("length_ms", view.length_ms)
                                   .Close();
    }
    const bool song_started = view.in_game && !was_in_game_;
    const bool song_ended = !view.in_game && was_in_game_;
    was_in_game_ = view.in_game;

    std::vector<Entry> entries;
    auto add = [&](std::string_view entity, std::string payload, Rate rate = Rate::kAny) {
        entries.push_back({StateTopic(id, entity), std::move(payload), rate});
    };
    auto add_attributes = [&](std::string_view entity, std::string payload,
                              Rate rate = Rate::kAny) {
        entries.push_back({AttributesTopic(id, entity), std::move(payload), rate});
    };
    add("song", view.title);
    add_attributes("song", JsonObject()
                               .Text("shortname", view.shortname)
                               .Text("artist", view.artist)
                               .Number("length_ms", view.length_ms)
                               .Close());
    add("artist", view.artist);
    add("venue", view.venue);
    add("score", std::to_string(view.score));
    add("playing", OnOff(view.in_game));
    add("paused", OnOff(view.paused));
    add("screen", view.screen);
    add("progress", progress_, Rate::kProgress);
    add_attributes("progress", progress_attributes_, Rate::kProgress);
    add("band", view.band);
    if (options_.stagekit) {
        const std::pair<const char*, uint8_t> colours[] = {
            {"stagekit_red", stagekit.red},
            {"stagekit_yellow", stagekit.yellow},
            {"stagekit_green", stagekit.green},
            {"stagekit_blue", stagekit.blue},
        };
        for (const auto& [entity, mask] : colours) {
            add(entity, std::to_string(std::popcount(mask)), Rate::kStageKit);
            add_attributes(entity, JsonObject().Number("mask", mask).Close(), Rate::kStageKit);
        }
        add("stagekit_strobe", stagekit.strobe ? std::to_string(stagekit.strobe) : "off",
            Rate::kStageKit);
        add("stagekit_fog", OnOff(stagekit.fog), Rate::kStageKit);
    }

    // Every state is retained, so HA has them all again when it restarts while
    // band3 runs; the retained "offline" makes them unavailable once band3 is
    // gone. An empty one (no song yet) deletes the topic's retained message
    // instead, so HA shows that state as unknown after a restart: as good.
    std::vector<mqtt::Message> out;
    if (fresh) {
        // a new connection: HA may have restarted or never heard of us, so everything
        out = Discovery(options_);
        if (!options_.stagekit) {
            // an empty retained config removes the entity, so the Stage Kit's
            // don't linger in HA, unavailable, after it was turned off
            for (const Entity& entity : kStageKitEntities) {
                out.push_back({ConfigTopic(options_, id, entity), "", true});
            }
        }
        out.push_back({StatusTopic(options_), "online", true});
        sent_.clear();
        stagekit_sent_.clear();
        for (Entry& entry : entries) {
            if (entry.rate == Rate::kStageKit) stagekit_sent_[entry.topic] = now;
            sent_[entry.topic] = entry.payload;
            out.push_back({std::move(entry.topic), std::move(entry.payload), true});
        }
        progress_sent_ = now;
        return out;
    }

    // a song's last values go even within the second, so HA isn't left with
    // where it was up to a second before the end
    const bool progress_due =
        song_ended || (view.in_game && (song_started || !progress_sent_ ||
                                        now - *progress_sent_ >= kProgressInterval));
    bool progress_went = false;
    for (Entry& entry : entries) {
        auto sent = sent_.find(entry.topic);
        if (sent != sent_.end() && sent->second == entry.payload) continue;
        if (entry.rate == Rate::kProgress) {
            if (!progress_due) continue;
            progress_went = true;
        } else if (entry.rate == Rate::kStageKit) {
            // held back, not dropped: it differs from what was sent until it goes
            auto last = stagekit_sent_.find(entry.topic);
            if (last != stagekit_sent_.end() && now - last->second < kStageKitInterval) continue;
            stagekit_sent_[entry.topic] = now;
        }
        sent_[entry.topic] = entry.payload;
        out.push_back({std::move(entry.topic), std::move(entry.payload), true});
    }
    if (progress_went) progress_sent_ = now;
    return out;
}

std::vector<std::string> WebhookWatcher::Changes(const GameView& view) {
    std::vector<std::string> payloads;
    if (started_) {
        // the song first, so an automation keyed on "playing" already knows it
        if (view.shortname != shortname_ && !view.shortname.empty()) {
            payloads.push_back(
                JsonObject().Text("type", "song").Text("name", view.title).Close());
        }
        if (view.in_game != in_game_) {
            payloads.push_back(JsonObject()
                                   .Text("type", "state")
                                   .Text("status", view.in_game ? "playing" : "menu")
                                   .Close());
        }
    }
    started_ = true;
    shortname_ = view.shortname;
    in_game_ = view.in_game;
    return payloads;
}

}  // namespace band3::ha
