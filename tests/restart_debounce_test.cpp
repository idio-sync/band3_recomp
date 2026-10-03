// Checks when a driver restarts for changed settings (src/Input/restart_debounce.h):
// once they have stayed the same for the settle time, not at every change.

#include <doctest/doctest.h>
#include <chrono>
#include <string>
#include "src/Input/restart_debounce.h"

using namespace band3::input;
using namespace std::chrono_literals;

namespace {

using Debounce = RestartDebounce<std::string>;
const Debounce::Clock::time_point t0{};

}

TEST_CASE("nothing restarts while the settings are the running ones") {
    Debounce debounce(300ms);
    CHECK_FALSE(debounce.Due("loopMIDI", "loopMIDI", t0));
    CHECK_FALSE(debounce.Due("loopMIDI", "loopMIDI", t0 + 1s));
}

TEST_CASE("a change restarts once it has stayed the same for the settle time") {
    Debounce debounce(300ms);
    std::string running = "a";
    CHECK_FALSE(debounce.Due("b", running, t0));
    CHECK_FALSE(debounce.Due("b", running, t0 + 299ms));
    CHECK(debounce.Due("b", running, t0 + 300ms));
    running = "b";
    CHECK_FALSE(debounce.Due("b", running, t0 + 316ms));
}

TEST_CASE("each change starts the wait again, so a run of changes restarts once") {
    Debounce debounce(300ms);
    const std::string running = "a";
    // a value changed every frame for a while
    int due = 0;
    for (int frame = 0; frame < 30; frame++) {
        const std::string wanted = "v" + std::to_string(frame);
        due += debounce.Due(wanted, running, t0 + frame * 16ms) ? 1 : 0;
    }
    CHECK(due == 0);
    CHECK_FALSE(debounce.Due("v29", running, t0 + 29 * 16ms + 299ms));
    CHECK(debounce.Due("v29", running, t0 + 29 * 16ms + 300ms));
}

TEST_CASE("a change put back before it settles restarts nothing") {
    Debounce debounce(300ms);
    CHECK_FALSE(debounce.Due("b", "a", t0));
    CHECK_FALSE(debounce.Due("a", "a", t0 + 100ms));
    // changed again later: waits the whole settle time from then
    CHECK_FALSE(debounce.Due("b", "a", t0 + 350ms));
    CHECK_FALSE(debounce.Due("b", "a", t0 + 600ms));
    CHECK(debounce.Due("b", "a", t0 + 650ms));
}
