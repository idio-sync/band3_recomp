// Checks when a song counts as being played (src/Input/song_state.cpp): the
// mouse in menus and a MIDI keyboard's lowest octave stop then, and only then.

#include <doctest/doctest.h>
#include "src/Input/song_state.h"

using namespace band3::input;

namespace {

void Set(bool on_screen, bool paused) {
    SetSongOnScreen(on_screen);
    SetSongPaused(paused);
}

}

TEST_CASE("a song is played only on a gameplay screen, unpaused") {
    Set(true, false);
    CHECK(SongBeingPlayed());
    CHECK(CurrentKeysMode() == midi_keys::Mode::kPlaying);

    // its pause menu
    Set(true, true);
    CHECK_FALSE(SongBeingPlayed());
    CHECK(CurrentKeysMode() == midi_keys::Mode::kMenus);

    // its loading screen, its results, the menus
    Set(false, false);
    CHECK_FALSE(SongBeingPlayed());
    CHECK(CurrentKeysMode() == midi_keys::Mode::kMenus);

    Set(false, true);
    CHECK_FALSE(SongBeingPlayed());
    Set(false, false);
}
