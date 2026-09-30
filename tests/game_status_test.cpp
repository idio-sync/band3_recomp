// Checks what the Instrument Lab keeps of the game's own decisions: the mic
// each player sings through (src/Audio/mic_mapping_status.cpp), logged only
// when it changes, and the per-controller lag table
// (src/Input/joypad_lag_status.cpp), shown only once a row is complete, with
// the joypad_lag setting's overrides.

#include <doctest/doctest.h>
#include <string>
#include <vector>
#include "src/Audio/mic_mapping_status.h"
#include "src/Input/joypad_lag_status.h"

using namespace band3;

TEST_CASE("the mic mapping reports only changes") {
    const std::vector<audio::MicMappingMic> one_mic{{0, true}};
    const std::vector<audio::MicMappingPlayer> first_has_it{{0, -1}, {-1, -1}};

    CHECK(!audio::GetMicMapping().seen);
    CHECK(audio::RecordMicMapping(first_has_it, one_mic));
    CHECK(!audio::RecordMicMapping(first_has_it, one_mic));

    const std::vector<audio::MicMappingPlayer> second_wants_it{{0, -1}, {-1, 0}};
    CHECK(audio::RecordMicMapping(second_wants_it, one_mic));
    CHECK(audio::RecordMicMapping(second_wants_it, {{0, false}}));

    const audio::MicMapping mapping = audio::GetMicMapping();
    CHECK(mapping.seen);
    CHECK(mapping.refreshes == 4);
    REQUIRE(mapping.players.size() == 2);
    CHECK(mapping.players[1].preferred == 0);
    REQUIRE(mapping.mics.size() == 1);
    CHECK(!mapping.mics[0].locked);
}

TEST_CASE("a lag row shows once every context is in") {
    constexpr uint32_t kXboxGuitar = 5;
    CHECK(!input::JoypadLagFor(kXboxGuitar));
    for (int c = 0; c < input::kLagContexts - 1; c++) {
        input::RecordJoypadLag(kXboxGuitar, c, 40.0f + c, 40.0f + c);
    }
    CHECK(!input::JoypadLagFor(kXboxGuitar));
    input::RecordJoypadLag(kXboxGuitar, input::kLagContexts - 1, 45.0f, 0.0f);

    const auto lag = input::JoypadLagFor(kXboxGuitar);
    REQUIRE(lag);
    CHECK(lag->game[1] == 41.0f);
    CHECK(lag->game[input::kLagContexts - 1] == 45.0f);
    CHECK(lag->used[input::kLagContexts - 1] == 0.0f);

    // out of the table: ignored, not written past it
    input::RecordJoypadLag(input::kLagJoypadTypes, 0, 1.0f, 1.0f);
    input::RecordJoypadLag(kXboxGuitar, input::kLagContexts, 1.0f, 1.0f);
    CHECK(!input::JoypadLagFor(input::kLagJoypadTypes));
    CHECK(input::JoypadLagFor(kXboxGuitar)->game[0] == 40.0f);
}

TEST_CASE("joypad_lag reads type=ms and type=ms/video/audio") {
    input::JoypadLagOverrides o{};
    CHECK(input::ParseJoypadLagOverrides(" 5=20, 8=30/43/-1 ,,33=/12.5/ ", o).empty());

    REQUIRE(o[5]);
    CHECK(o[5]->game == 20.0f);
    CHECK(!o[5]->video_calibration);
    CHECK(!o[5]->audio_calibration);
    REQUIRE(o[8]);
    CHECK(o[8]->audio_calibration == -1.0f);
    REQUIRE(o[33]);
    CHECK(!o[33]->game);
    CHECK(o[33]->video_calibration == 12.5f);
    CHECK(!o[6]);

    const auto bad = input::ParseJoypadLagOverrides(
        "47=10,x=10,5=,5=//,5=1/2/3/4,5=abc,5=2000,9", o);
    CHECK(bad == std::vector<std::string>{"47=10", "x=10", "5=", "5=//", "5=1/2/3/4", "5=abc",
                                          "5=2000", "9"});
    // bad entries leave what was read before alone
    CHECK(o[5]->game == 20.0f);
}

TEST_CASE("an override replaces only the contexts it sets") {
    input::JoypadLagOverrides o{};
    input::ParseJoypadLagOverrides("5=20,8=/30/", o);
    using input::ApplyJoypadLagOverride;

    // the game number covers play and the practice speeds
    CHECK(ApplyJoypadLagOverride(o, 5, input::kLagGame, 45.0f) == 20.0f);
    CHECK(ApplyJoypadLagOverride(o, 5, 6, 45.0f) == 20.0f);
    CHECK(ApplyJoypadLagOverride(o, 5, input::kLagVideoCalibration, 43.0f) == 43.0f);
    CHECK(ApplyJoypadLagOverride(o, 8, input::kLagVideoCalibration, 43.0f) == 30.0f);
    CHECK(ApplyJoypadLagOverride(o, 8, input::kLagGame, 36.0f) == 36.0f);
    CHECK(ApplyJoypadLagOverride(o, 9, input::kLagGame, 36.0f) == 36.0f);
    CHECK(ApplyJoypadLagOverride(o, input::kLagJoypadTypes, input::kLagGame, 14.0f) == 14.0f);
}
