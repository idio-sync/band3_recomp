#pragma once
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "mqtt_protocol.h"

// What band3 tells Home Assistant, without a connection: the MQTT discovery
// configs of its entities, their state topics and what changed since they were
// last published, the Stage Kit's lights as the game sets them, and the RB3E
// Dashboard's webhook payloads. home_assistant.cpp puts it on the wire.
namespace band3::ha {

// the PC's name as a topic and id part: lower-case ASCII letters and digits
// kept, everything else '_' (so "Living Room PC" is "living_room_pc"); "pc"
// when the name is empty
std::string PcId(std::string_view pc_name);

// The Stage Kit's lights: which of each colour's eight LEDs are lit, the
// strobe's speed and the fog machine.
struct StageKit {
    uint8_t red = 0, yellow = 0, green = 0, blue = 0;
    uint8_t strobe = 0;  // 0 off, 1-4
    bool fog = false;
};

// one StageKit::SetState(left, right) command applied, as RB3E's protocol reads
// it: `right` says what to set and `left` is the colour's LED mask
StageKit ApplyStageKit(StageKit state, uint8_t left, uint8_t right);
// in one word, so the game thread can update it with a compare-and-swap
uint64_t PackStageKit(const StageKit& state);
StageKit UnpackStageKit(uint64_t packed);

// what HA is told, already UTF-8 (the caller converts the game's strings)
struct GameView {
    std::string screen;
    bool in_game = false;
    bool paused = false;
    std::string title, artist, shortname;
    int32_t length_ms = 0;
    int32_t song_ms = -1;
    std::string venue;
    int64_t score = 0;
    std::string band;  // discord::detail::BandText's text
};

struct Options {
    std::string pc_id;  // PcId(pc_name); taken from pc_name when left empty
    std::string pc_name;
    std::string discovery_prefix = "homeassistant";
    bool stagekit = false;
    std::string sw_version;  // left out of the device when empty
};

// band3/<id>/status: "online" / "offline", retained, and the connection's will
std::string StatusTopic(const Options& options);
// every entity's config, retained, so HA finds them again after it restarts
std::vector<mqtt::Message> Discovery(const Options& options);

// Tracks what was published and decides what to publish next.
class Publisher {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr std::chrono::seconds kProgressInterval{1};
    static constexpr std::chrono::milliseconds kStageKitInterval{50};

    explicit Publisher(Options options);

    // fresh: discovery + "online" + every state, as after a (re)connect; otherwise only
    // changed payloads, progress (and its attributes) at most once a second, each Stage Kit
    // topic at most once per 50 ms, a held-back value sent once its interval has passed
    std::vector<mqtt::Message> Changes(const GameView& view, const StageKit& stagekit,
                                       Clock::time_point now, bool fresh);

private:
    Options options_;
    // topic -> the payload last published to it
    std::map<std::string, std::string> sent_;
    // progress is only worked out in a song; out of one it keeps these
    std::string progress_ = "0";
    std::string progress_attributes_;
    bool was_in_game_ = false;
    std::optional<Clock::time_point> progress_sent_;
    // Stage Kit topic -> when it was last published
    std::map<std::string, Clock::time_point> stagekit_sent_;
};

// The dashboard's webhook payloads, from state changes (JSON strings, in the order to POST).
class WebhookWatcher {
public:
    std::vector<std::string> Changes(const GameView& view);  // first call only sets the baseline

private:
    bool started_ = false;
    bool in_game_ = false;
    std::string shortname_;
};

}  // namespace band3::ha
