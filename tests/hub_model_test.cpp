// Checks the launcher hub's model (src/Launcher/ui/hub_model.h): its menu
// with the game found and missing, and the status card's facts read from the
// game data and the user data: Deluxe's version, the last song played, and
// the icon each instrument shows as.

#include <doctest/doctest.h>
#include <cstdint>
#include <ostream>
#include <string>
#include <vector>
#include "src/Launcher/ui/hub_model.h"

using namespace band3::launcher::ui;

namespace {

void Put32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; i++) out.push_back(uint8_t(v >> (8 * i)));
}

// a decrypted .dtb's bytes around a key and its string node, as dx_version.dtb has them
std::vector<uint8_t> Dtb(std::string_view key, std::string_view value) {
    std::vector<uint8_t> out = {0x01, 0x02, 0x00, 0x00};
    Put32(out, 0x05);  // a symbol node
    Put32(out, uint32_t(key.size()));
    out.insert(out.end(), key.begin(), key.end());
    Put32(out, 0x12);  // a string node
    Put32(out, uint32_t(value.size()));
    out.insert(out.end(), value.begin(), value.end());
    out.push_back(0x00);
    return out;
}

}

TEST_CASE("the hub's menu starts with Play, or with finding the game when it's missing") {
    const auto found = HubMenu(true);
    REQUIRE(found.size() == 5);
    CHECK(found[0].item == HubItem::kPlay);
    CHECK(found[0].enabled);
    CHECK(found[4].item == HubItem::kQuit);
    const auto missing = HubMenu(false);
    REQUIRE(missing.size() == 6);
    CHECK(missing[0].item == HubItem::kFindGame);
    CHECK(missing[1].item == HubItem::kPlay);
    CHECK_FALSE(missing[1].enabled);
    CHECK(HubItemName(HubItem::kTestControllers) == "Test Controllers");
}

TEST_CASE("Deluxe's version is read from its dx_version file") {
    CHECK(DeluxeVersion(Dtb("rb3dx_version", "r1736+84a0b08")) == "r1736");
    // the whole string where there's no commit after it
    CHECK(DeluxeVersion(Dtb("rb3dx_version", "1.1.6")) == "1.1.6");
    CHECK_FALSE(DeluxeVersion(Dtb("something_else", "r1")));
    CHECK_FALSE(DeluxeVersion({}));
    // a length running past the end isn't read
    std::vector<uint8_t> torn = Dtb("rb3dx_version", "r1736");
    torn.resize(torn.size() - 4);
    CHECK_FALSE(DeluxeVersion(torn));
}

TEST_CASE("the last song played is the history's last line that reads") {
    const std::string tail =
        "{\"v\":1,\"t\":100,\"shortname\":\"a\",\"song_id\":1,\"title\":\"First\",\"artist\":\"One\","
        "\"part\":\"guitar\",\"diff\":3,\"score\":1,\"stars\":5,\"pct\":90,\"streak\":1,\"od_deploys\":0,"
        "\"band_size\":1,\"speed\":100,\"modifiers\":[]}\n"
        "{\"v\":1,\"t\":200,\"shortname\":\"b\",\"song_id\":2,\"title\":\"20th Century Boy\","
        "\"artist\":\"T. Rex\",\"part\":\"drum\",\"diff\":3,\"score\":1,\"stars\":4,\"pct\":80,"
        "\"streak\":1,\"od_deploys\":0,\"band_size\":1,\"speed\":100,\"modifiers\":[]}\n"
        "{\"v\":1,\"t\":300,\"title\":\"torn";
    const auto last = LastPlayed(tail);
    REQUIRE(last);
    CHECK(last->title == "20th Century Boy");
    CHECK(last->artist == "T. Rex");
    CHECK_FALSE(LastPlayed(""));
    CHECK_FALSE(LastPlayed("not json\n"));
}

TEST_CASE("each controller shows as its instrument's icon") {
    // the 4x4 sheet: bass, guitar, drums, keys; then the pro ones; the mics
    CHECK(IconFor(6) == IconCell{1, 0});   // a guitar
    CHECK(IconFor(8) == IconCell{2, 0});   // drums
    CHECK(IconFor(15) == IconCell{3, 0});  // a keytar
    CHECK(IconFor(25) == IconCell{1, 1});  // a pro guitar
    // a gamepad has none of its own
    CHECK_FALSE(IconFor(1));
    CHECK(kVocalsIcon == IconCell{0, 2});
}
