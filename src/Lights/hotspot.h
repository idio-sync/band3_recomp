#pragma once
#include <string>

// The Steam Deck's own Wi-Fi network (wifi_hotspot), for the wireless Stage
// Kits (Pico W) to join where there's no other Wi-Fi: NetworkManager runs it,
// through nmcli, while band3 runs and the setting is on. It takes the Deck's
// Wi-Fi off the network it was on, which NetworkManager joins again once the
// hotspot stops. Only on a Steam Deck running Linux, and not under the test
// harness; elsewhere nothing starts. nmcli runs on a thread of its own.
namespace band3::lights::hotspot {

// with lights::Start and Stop
void Start();
// takes the hotspot down if band3 started it; at exit
void Stop();

// takes wifi_hotspot, its name and its password as they are now, from the UI
// thread (the cvars' text isn't safe to read from another): at Start, and as
// the Settings screen changes them
void Follow();

struct Status {
    enum class State { kOff, kStarting, kOn, kProblem };
    State state = State::kOff;
    // kOn: the network's name and the Deck's address on it ("10.42.0.1");
    // kProblem: what's wrong, in words
    std::string name;
    std::string address;
    // kOn: with no DHCP (hotspot_plan.h's kLinkLocal), so a device that joins
    // picks its own address
    bool link_local = false;
    std::string problem;
};
Status Current();

}
