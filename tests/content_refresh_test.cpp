// Checks when band3 has the game list content again for packages added while
// it runs (src/Content/content_refresh.cpp).

#include <doctest/doctest.h>
#include <string>
#include "src/Content/content_refresh.h"

using namespace band3::content;

TEST_CASE("only the main hub and the Music Library refresh, as their own scripts do") {
    CHECK(RefreshScript("main_hub_screen") ==
          "{if {content_mgr refresh_done} {content_mgr start_refresh}}");
    CHECK(RefreshScript("song_select_screen") == "{song_select_panel storage_changed}");
    CHECK(RefreshScript("game_screen").empty());
    CHECK(RefreshScript("part_difficulty_screen").empty());
    CHECK(RefreshScript("").empty());
}

TEST_CASE("nothing new for the game, or a song playing: no refresh") {
    RefreshPlanner planner;
    CHECK(planner.Next(0, 0, "main_hub_screen", false).empty());
    CHECK(planner.Next(3, 3, "main_hub_screen", false).empty());
    CHECK(planner.Next(1, 0, "song_select_screen", true).empty());
}

TEST_CASE("new packages refresh once per safe screen, until the game lists them") {
    RefreshPlanner planner;
    // somewhere else: it waits
    CHECK(planner.Next(1, 0, "part_difficulty_screen", false).empty());
    // the Music Library: once, not every frame after
    CHECK(planner.Next(1, 0, "song_select_screen", false) == "{song_select_panel storage_changed}");
    CHECK(planner.Next(1, 0, "song_select_screen", false).empty());
    // left and come back to: another try
    CHECK(planner.Next(1, 0, "part_difficulty_screen", false).empty());
    CHECK(!planner.Next(1, 0, "song_select_screen", false).empty());
    // more packages on the same screen: another try
    CHECK(!planner.Next(2, 0, "song_select_screen", false).empty());
    // listed: nothing more
    CHECK(planner.Next(2, 2, "main_hub_screen", false).empty());
}
