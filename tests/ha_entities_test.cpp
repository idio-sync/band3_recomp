// Checks what band3 tells Home Assistant (src/Net/ha_entities.cpp): the MQTT
// discovery configs, the state topics and what changes publish, the rates
// progress and the Stage Kit are held to, the Stage Kit's decoding and the
// RB3E Dashboard's webhook payloads.

#include <doctest/doctest.h>
#include <optional>
#include <string>
#include <vector>
#include "src/Net/ha_entities.h"

using namespace band3::ha;
using band3::mqtt::Message;
using namespace std::chrono_literals;

namespace {

using Clock = Publisher::Clock;

// a fake clock: `ms` after an arbitrary start
Clock::time_point At(int64_t ms) { return Clock::time_point{} + std::chrono::milliseconds(ms); }

Options TestOptions(bool stagekit = false) {
    Options options;
    options.pc_id = "living_room_pc";
    options.pc_name = "Living Room PC";
    options.stagekit = stagekit;
    options.sw_version = "v1.2";
    return options;
}

// the payload published to `topic`, if it was
std::optional<std::string> Find(const std::vector<Message>& messages, const std::string& topic) {
    for (const Message& message : messages) {
        if (message.topic == topic) return message.payload;
    }
    return std::nullopt;
}

const Message* FindMessage(const std::vector<Message>& messages, const std::string& topic) {
    for (const Message& message : messages) {
        if (message.topic == topic) return &message;
    }
    return nullptr;
}

std::optional<std::string> State(const std::vector<Message>& messages, const std::string& entity) {
    return Find(messages, "band3/living_room_pc/" + entity);
}

GameView Menus() {
    GameView view;
    view.screen = "main_hub_screen";
    return view;
}

GameView Playing(int32_t song_ms) {
    GameView view;
    view.screen = "game_screen";
    view.in_game = true;
    view.title = "Free Bird";
    view.artist = "Lynyrd Skynyrd";
    view.shortname = "freebird";
    view.length_ms = 200000;
    view.song_ms = song_ms;
    view.venue = "Small Club";
    view.score = 12345;
    view.band = "Guitar (Expert)";
    return view;
}

}  // namespace

TEST_CASE("ha: the PC id is the PC's name made safe for topics") {
    CHECK(PcId("Living Room PC") == "living_room_pc");
    CHECK(PcId("DESKTOP-AB12") == "desktop_ab12");
    CHECK(PcId("") == "pc");
    // each byte of a UTF-8 character becomes '_'
    CHECK(PcId("Caf\xc3\xa9") == "caf__");
}

TEST_CASE("ha: a sensor's discovery config") {
    const std::vector<Message> configs = Discovery(TestOptions());
    const Message* song =
        FindMessage(configs, "homeassistant/sensor/band3_living_room_pc/song/config");
    REQUIRE(song);
    CHECK(song->retain);
    CHECK(song->payload ==
          R"json({"name":"Song","unique_id":"band3_living_room_pc_song",)json"
          R"json("state_topic":"band3/living_room_pc/song",)json"
          R"json("json_attributes_topic":"band3/living_room_pc/song/attributes",)json"
          R"json("icon":"mdi:music-note","availability_topic":"band3/living_room_pc/status",)json"
          R"json("payload_available":"online","payload_not_available":"offline",)json"
          R"json("device":{"identifiers":["band3_living_room_pc"],"name":"band3 (Living Room PC)",)json"
          R"json("manufacturer":"band3","model":"Rock Band 3","sw_version":"v1.2"}})json");
}

TEST_CASE("ha: a binary sensor's discovery config") {
    const std::vector<Message> configs = Discovery(TestOptions());
    const Message* playing =
        FindMessage(configs, "homeassistant/binary_sensor/band3_living_room_pc/playing/config");
    REQUIRE(playing);
    CHECK(playing->payload ==
          R"json({"name":"Playing","unique_id":"band3_living_room_pc_playing",)json"
          R"json("state_topic":"band3/living_room_pc/playing","payload_on":"ON","payload_off":"OFF",)json"
          R"json("device_class":"running","availability_topic":"band3/living_room_pc/status",)json"
          R"json("payload_available":"online","payload_not_available":"offline",)json"
          R"json("device":{"identifiers":["band3_living_room_pc"],"name":"band3 (Living Room PC)",)json"
          R"json("manufacturer":"band3","model":"Rock Band 3","sw_version":"v1.2"}})json");
}

TEST_CASE("ha: a Stage Kit sensor's discovery config, without a version") {
    Options options = TestOptions(true);
    options.sw_version.clear();
    options.discovery_prefix = "ha";
    const std::vector<Message> configs = Discovery(options);
    const Message* red = FindMessage(configs, "ha/sensor/band3_living_room_pc/stagekit_red/config");
    REQUIRE(red);
    CHECK(red->payload ==
          R"json({"name":"Stage Kit red","unique_id":"band3_living_room_pc_stagekit_red",)json"
          R"json("state_topic":"band3/living_room_pc/stagekit_red",)json"
          R"json("json_attributes_topic":"band3/living_room_pc/stagekit_red/attributes",)json"
          R"json("availability_topic":"band3/living_room_pc/status",)json"
          R"json("payload_available":"online","payload_not_available":"offline",)json"
          R"json("device":{"identifiers":["band3_living_room_pc"],"name":"band3 (Living Room PC)",)json"
          R"json("manufacturer":"band3","model":"Rock Band 3"}})json");
    CHECK(FindMessage(configs, "ha/binary_sensor/band3_living_room_pc/stagekit_fog/config"));
}

TEST_CASE("ha: discovery has every entity, retained, and the Stage Kit's only when asked") {
    const std::vector<Message> plain = Discovery(TestOptions(false));
    const std::vector<Message> with = Discovery(TestOptions(true));
    CHECK(plain.size() == 9);
    CHECK(with.size() == 15);
    for (const Message& message : with) CHECK(message.retain);
    CHECK_FALSE(FindMessage(plain, "homeassistant/sensor/band3_living_room_pc/stagekit_red/config"));
    CHECK(StatusTopic(TestOptions()) == "band3/living_room_pc/status");
    // the id comes from the name when it isn't given
    Options options;
    options.pc_name = "Den";
    CHECK(StatusTopic(options) == "band3/den/status");
}

TEST_CASE("ha: a fresh connection gets discovery, online, then every state, all retained") {
    Publisher publisher(TestOptions());
    const std::vector<Message> out = publisher.Changes(Menus(), {}, At(0), true);
    // the 9 configs, the 6 Stage Kit configs emptied, online, 11 states
    REQUIRE(out.size() == 9 + 6 + 1 + 11);
    for (const Message& message : out) CHECK(message.retain);
    CHECK(out[15].topic == "band3/living_room_pc/status");
    CHECK(out[15].payload == "online");
    CHECK(State(out, "song") == "");
    CHECK(State(out, "song/attributes") == R"({"shortname":"","artist":"","length_ms":0})");
    CHECK(State(out, "playing") == "OFF");
    CHECK(State(out, "paused") == "OFF");
    CHECK(State(out, "screen") == "main_hub_screen");
    CHECK(State(out, "score") == "0");
    CHECK(State(out, "progress") == "0");
    CHECK(State(out, "progress/attributes") == R"({"position_ms":0,"length_ms":0})");

    // and with the Stage Kit on, its configs and states too, none emptied
    Publisher stagekit(TestOptions(true));
    const std::vector<Message> with = stagekit.Changes(Menus(), {}, At(0), true);
    CHECK(with.size() == 15 + 1 + 11 + 10);
    for (const Message& message : with) {
        const bool emptied = message.payload.empty() && message.topic.ends_with("/config");
        CHECK_FALSE(emptied);
    }
}

TEST_CASE("ha: with the Stage Kit off, a fresh connection removes its entities from HA") {
    Options options = TestOptions(false);
    options.discovery_prefix = "ha";
    Publisher publisher(options);
    const std::vector<Message> out = publisher.Changes(Menus(), {}, At(0), true);
    const char* removed[] = {
        "ha/sensor/band3_living_room_pc/stagekit_red/config",
        "ha/sensor/band3_living_room_pc/stagekit_yellow/config",
        "ha/sensor/band3_living_room_pc/stagekit_green/config",
        "ha/sensor/band3_living_room_pc/stagekit_blue/config",
        "ha/sensor/band3_living_room_pc/stagekit_strobe/config",
        "ha/binary_sensor/band3_living_room_pc/stagekit_fog/config",
    };
    for (const char* topic : removed) {
        CAPTURE(topic);
        const Message* message = FindMessage(out, topic);
        REQUIRE(message);
        // an empty retained message deletes the retained config
        CHECK(message->payload.empty());
        CHECK(message->retain);
    }
    // nothing of the Stage Kit's states
    CHECK_FALSE(State(out, "stagekit_red"));
    // and again on every fresh connection, but not in between
    CHECK(FindMessage(publisher.Changes(Menus(), {}, At(100), true), removed[0]));
    CHECK(publisher.Changes(Menus(), {}, At(200), false).empty());
}

TEST_CASE("ha: an unchanged view publishes nothing; fresh sends everything again") {
    Publisher publisher(TestOptions());
    publisher.Changes(Menus(), {}, At(0), true);
    CHECK(publisher.Changes(Menus(), {}, At(50), false).empty());
    CHECK(publisher.Changes(Menus(), {}, At(5000), false).empty());
    CHECK(publisher.Changes(Menus(), {}, At(5050), true).size() == 27);
    CHECK(publisher.Changes(Menus(), {}, At(5100), false).empty());
}

TEST_CASE("ha: the states through a song") {
    Publisher publisher(TestOptions());
    publisher.Changes(Menus(), {}, At(0), true);

    // a song starts: everything about it, and progress at once
    std::vector<Message> out = publisher.Changes(Playing(-1), {}, At(100), false);
    // retained, so HA has them again after it restarts
    for (const Message& message : out) CHECK(message.retain);
    CHECK(State(out, "song") == "Free Bird");
    CHECK(State(out, "song/attributes") ==
          R"({"shortname":"freebird","artist":"Lynyrd Skynyrd","length_ms":200000})");
    CHECK(State(out, "artist") == "Lynyrd Skynyrd");
    CHECK(State(out, "venue") == "Small Club");
    CHECK(State(out, "score") == "12345");
    CHECK(State(out, "playing") == "ON");
    CHECK(State(out, "screen") == "game_screen");
    CHECK(State(out, "band") == "Guitar (Expert)");
    CHECK_FALSE(State(out, "paused"));  // unchanged
    CHECK_FALSE(State(out, "progress"));  // still 0
    CHECK(State(out, "progress/attributes") == R"({"position_ms":0,"length_ms":200000})");

    // halfway, a second later
    out = publisher.Changes(Playing(100000), {}, At(1100), false);
    CHECK(State(out, "progress") == "50");
    CHECK(State(out, "progress/attributes") == R"({"position_ms":100000,"length_ms":200000})");

    // paused
    GameView paused = Playing(100000);
    paused.paused = true;
    out = publisher.Changes(paused, {}, At(1200), false);
    REQUIRE(out.size() == 1);
    CHECK(State(out, "paused") == "ON");
    out = publisher.Changes(Playing(100000), {}, At(1300), false);
    CHECK(State(out, "paused") == "OFF");

    // past the end clamps to 100
    out = publisher.Changes(Playing(250000), {}, At(2300), false);
    CHECK(State(out, "progress") == "100");

    // after the song: song, artist, score and progress keep their last value
    GameView after = Menus();
    after.title = "Free Bird";
    after.artist = "Lynyrd Skynyrd";
    after.shortname = "freebird";
    after.length_ms = 200000;
    after.score = 12345;
    after.venue = "Small Club";
    after.band = "Guitar (Expert)";
    out = publisher.Changes(after, {}, At(5000), false);
    CHECK(State(out, "playing") == "OFF");
    CHECK(State(out, "screen") == "main_hub_screen");
    CHECK_FALSE(State(out, "song"));
    CHECK_FALSE(State(out, "score"));
    CHECK_FALSE(State(out, "progress"));
    CHECK_FALSE(State(out, "progress/attributes"));

    // and a reconnect out of a song still sends progress as it was left
    out = publisher.Changes(after, {}, At(6000), true);
    CHECK(State(out, "progress") == "100");
    CHECK(State(out, "progress/attributes") == R"({"position_ms":250000,"length_ms":200000})");
}

TEST_CASE("ha: progress goes out at most once a second, and only in a song") {
    Publisher publisher(TestOptions());
    publisher.Changes(Menus(), {}, At(0), true);

    // the song's first value goes at once
    std::vector<Message> out = publisher.Changes(Playing(2000), {}, At(10000), false);
    CHECK(State(out, "progress") == "1");
    // half a second on, held
    out = publisher.Changes(Playing(4000), {}, At(10500), false);
    CHECK_FALSE(State(out, "progress"));
    CHECK_FALSE(State(out, "progress/attributes"));
    // a second on, sent
    out = publisher.Changes(Playing(6000), {}, At(11000), false);
    CHECK(State(out, "progress") == "3");
    CHECK(State(out, "progress/attributes") == R"({"position_ms":6000,"length_ms":200000})");

    // out of the song the song clock is ignored
    GameView menus = Menus();
    menus.song_ms = 150000;
    menus.length_ms = 200000;
    out = publisher.Changes(menus, {}, At(13000), false);
    CHECK_FALSE(State(out, "progress"));
    CHECK_FALSE(State(out, "progress/attributes"));

    // the next song shows 0 at once, though the last value went out just now
    out = publisher.Changes(Playing(-1), {}, At(13100), false);
    CHECK(State(out, "progress") == "0");
}

TEST_CASE("ha: a song's last progress goes out when it ends, even within the second") {
    Publisher publisher(TestOptions());
    publisher.Changes(Menus(), {}, At(0), true);
    std::vector<Message> out = publisher.Changes(Playing(2000), {}, At(10000), false);
    CHECK(State(out, "progress") == "1");
    // held back: within the second
    out = publisher.Changes(Playing(196000), {}, At(10500), false);
    CHECK_FALSE(State(out, "progress"));

    // the song ends 100 ms later: the values it ended on go at once
    GameView after = Playing(196000);
    after.in_game = false;
    after.screen = "main_hub_screen";
    out = publisher.Changes(after, {}, At(10600), false);
    CHECK(State(out, "playing") == "OFF");
    CHECK(State(out, "progress") == "98");
    CHECK(State(out, "progress/attributes") == R"({"position_ms":196000,"length_ms":200000})");

    // once only: out of the song nothing more
    out = publisher.Changes(after, {}, At(10700), false);
    CHECK(out.empty());
}

TEST_CASE("ha: Stage Kit commands") {
    StageKit kit;
    kit = ApplyStageKit(kit, 0x81, 0x20);
    CHECK(kit.blue == 0x81);
    kit = ApplyStageKit(kit, 0x0F, 0x40);
    CHECK(kit.green == 0x0F);
    kit = ApplyStageKit(kit, 0xF0, 0x60);
    CHECK(kit.yellow == 0xF0);
    kit = ApplyStageKit(kit, 0xFF, 0x80);
    CHECK(kit.red == 0xFF);
    kit = ApplyStageKit(kit, 0, 0x01);
    CHECK(kit.fog);
    kit = ApplyStageKit(kit, 0, 0x02);
    CHECK_FALSE(kit.fog);
    for (uint8_t right = 0x03; right <= 0x06; right++) {
        kit = ApplyStageKit(kit, 0, right);
        CHECK(kit.strobe == right - 2);
    }
    kit = ApplyStageKit(kit, 0, 0x07);
    CHECK(kit.strobe == 0);

    // an unknown command changes nothing
    kit = ApplyStageKit(kit, 0, 0x01);
    kit = ApplyStageKit(kit, 0, 0x04);
    const StageKit before = kit;
    kit = ApplyStageKit(kit, 0x55, 0x10);
    CHECK(PackStageKit(kit) == PackStageKit(before));

    // 0xFF turns everything off
    kit = ApplyStageKit(kit, 0x12, 0xFF);
    CHECK(kit.red == 0);
    CHECK(kit.yellow == 0);
    CHECK(kit.green == 0);
    CHECK(kit.blue == 0);
    CHECK(kit.strobe == 0);
    CHECK_FALSE(kit.fog);
}

TEST_CASE("ha: the Stage Kit packs into one word and back") {
    const StageKit kit{0x01, 0x80, 0x5A, 0xFF, 4, true};
    const StageKit back = UnpackStageKit(PackStageKit(kit));
    CHECK(back.red == kit.red);
    CHECK(back.yellow == kit.yellow);
    CHECK(back.green == kit.green);
    CHECK(back.blue == kit.blue);
    CHECK(back.strobe == kit.strobe);
    CHECK(back.fog == kit.fog);
    CHECK(PackStageKit(StageKit{}) == 0);
    CHECK(PackStageKit(UnpackStageKit(PackStageKit(kit))) == PackStageKit(kit));
}

TEST_CASE("ha: Stage Kit states, at most once per 50 ms, ending on the latest") {
    Publisher publisher(TestOptions(true));
    StageKit kit;
    std::vector<Message> out = publisher.Changes(Menus(), kit, At(0), true);
    CHECK(State(out, "stagekit_red") == "0");
    CHECK(State(out, "stagekit_red/attributes") == R"({"mask":0})");
    CHECK(State(out, "stagekit_strobe") == "off");
    CHECK(State(out, "stagekit_fog") == "OFF");

    // 50 ms after the fresh send: goes at once
    kit = ApplyStageKit(kit, 0x07, 0x80);
    kit = ApplyStageKit(kit, 0, 0x05);
    kit = ApplyStageKit(kit, 0, 0x01);
    out = publisher.Changes(Menus(), kit, At(50), false);
    CHECK(State(out, "stagekit_red") == "3");
    CHECK(State(out, "stagekit_red/attributes") == R"({"mask":7})");
    CHECK(State(out, "stagekit_strobe") == "3");
    CHECK(State(out, "stagekit_fog") == "ON");

    // two more changes inside the interval are held back...
    kit = ApplyStageKit(kit, 0x01, 0x80);
    out = publisher.Changes(Menus(), kit, At(70), false);
    CHECK(out.empty());
    kit = ApplyStageKit(kit, 0xFF, 0x80);
    out = publisher.Changes(Menus(), kit, At(90), false);
    CHECK(out.empty());
    // ...and the latest goes once it has passed
    out = publisher.Changes(Menus(), kit, At(100), false);
    CHECK(State(out, "stagekit_red") == "8");
    CHECK(State(out, "stagekit_red/attributes") == R"({"mask":255})");
    CHECK(out.size() == 2);

    // a colour whose count stays the same still updates its mask
    kit = ApplyStageKit(kit, 0x01, 0x20);
    publisher.Changes(Menus(), kit, At(200), false);
    kit = ApplyStageKit(kit, 0x02, 0x20);
    out = publisher.Changes(Menus(), kit, At(300), false);
    CHECK_FALSE(State(out, "stagekit_blue"));
    CHECK(State(out, "stagekit_blue/attributes") == R"({"mask":2})");

    // each topic keeps its own interval, and a held Stage Kit topic doesn't
    // hold back the others
    kit = ApplyStageKit(kit, 0, 0xFF);
    out = publisher.Changes(Playing(-1), kit, At(310), false);
    CHECK(State(out, "playing") == "ON");
    CHECK(State(out, "stagekit_red") == "0");
    CHECK(State(out, "stagekit_blue") == "0");
    CHECK_FALSE(State(out, "stagekit_blue/attributes"));  // sent 10 ms ago
    out = publisher.Changes(Playing(-1), kit, At(350), false);
    REQUIRE(out.size() == 1);
    CHECK(State(out, "stagekit_blue/attributes") == R"({"mask":0})");
}

TEST_CASE("ha: the Stage Kit isn't published unless asked") {
    Publisher publisher(TestOptions(false));
    publisher.Changes(Menus(), {}, At(0), true);
    const StageKit kit{0xFF, 0, 0, 0, 2, true};
    CHECK(publisher.Changes(Menus(), kit, At(100), false).empty());
}

TEST_CASE("ha: webhook payloads") {
    WebhookWatcher watcher;
    // the first call is the baseline, even mid-song
    CHECK(watcher.Changes(Playing(1000)).empty());
    CHECK(watcher.Changes(Playing(2000)).empty());

    GameView menus = Menus();
    menus.shortname = "freebird";
    CHECK(watcher.Changes(menus) == std::vector<std::string>{R"({"type":"state","status":"menu"})"});
    CHECK(watcher.Changes(menus).empty());

    // a new song and its start in one call: the song first
    GameView next = Playing(-1);
    next.shortname = "quote";
    next.title = "Say \"Hi\"";
    CHECK(watcher.Changes(next) ==
          std::vector<std::string>{R"({"type":"song","name":"Say \"Hi\""})",
                                   R"({"type":"state","status":"playing"})"});
    CHECK(watcher.Changes(next).empty());

    // a song chosen in the menus, no song cleared
    WebhookWatcher menus_watcher;
    menus_watcher.Changes(Menus());
    GameView chosen = Menus();
    chosen.shortname = "freebird";
    chosen.title = "Free Bird";
    CHECK(menus_watcher.Changes(chosen) ==
          std::vector<std::string>{R"({"type":"song","name":"Free Bird"})"});
    CHECK(menus_watcher.Changes(Menus()).empty());
}
