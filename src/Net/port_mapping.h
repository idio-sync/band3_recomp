#pragma once
#include <cstdint>
#include <string>
#include <string_view>

// Opening liveless_port on the router, as RB3Enhanced does, so players over
// the internet reach this game without a hand-made port forward: PCP first,
// then NAT-PMP, then UPnP (miniupnpc). It also learns the router's public
// address, which XNetGetTitleXnAddr tells joining players when nothing better
// knows it. Runs on a thread of its own from online::Start; Stop deletes the
// mapping. The bytes are port_mapping_protocol.h's.
namespace band3::port_mapping {

enum class State {
    kOff,        // not started: liveless or liveless_port_mapping off, or the harness guard
    kSearching,  // asking the router
    kMapped,     // the router forwards the port to this PC
    kFailed,     // no way the router offers worked
};

enum class Method { kNone, kPcp, kNatPmp, kUpnp };

// As the test server's port_mapping_status and port_mapping= wait say it.
inline std::string_view StateName(State state) {
    switch (state) {
        case State::kOff: return "off";
        case State::kSearching: return "searching";
        case State::kMapped: return "mapped";
        case State::kFailed: return "failed";
    }
    return "off";
}

inline std::string_view MethodName(Method method) {
    switch (method) {
        case Method::kPcp: return "pcp";
        case Method::kNatPmp: return "natpmp";
        case Method::kUpnp: return "upnp";
        case Method::kNone: break;
    }
    return "";
}

// As players know them, for the Rooms panel.
inline std::string_view MethodLabel(Method method) {
    switch (method) {
        case Method::kPcp: return "PCP";
        case Method::kNatPmp: return "NAT-PMP";
        case Method::kUpnp: return "UPnP";
        case Method::kNone: break;
    }
    return "";
}

struct Status {
    State state = State::kOff;
    Method method = Method::kNone;  // what mapped it, or was last tried
    // the router's public address, network order, 0 until it says; a private
    // one (a router behind another) shows here but isn't advertised
    uint32_t external_ipv4 = 0;
    uint16_t port = 0;     // liveless_port, mapped the same inside and out
    uint32_t lease_s = 0;  // what the router granted; 0 for UPnP's permanent ones
    std::string error;     // why it isn't mapped, or empty
};

struct Config {
    uint16_t port = 0;
    // the overrides: liveless_gateway (host[:port], instead of the default
    // route's gateway on 5351) and liveless_upnp_url (a root description URL,
    // instead of SSDP discovery)
    std::string gateway;
    std::string upnp_url;
    // under the test harness (test_port set), each way runs only with its
    // override, so no test reaches the real router
    bool harness = false;
};

// From online::Start, before the game runs. Any thread may call the others.
void Start(const Config& config);
// Deletes the mapping (waiting about a second for the router), then ends the
// thread; a router that hasn't answered by then is left to let it lapse.
void Stop();
Status GetStatus();
// The router's public address while the port is mapped and the address is a
// public one, else 0; network order. Never locks: the game thread's
// XNetGetTitleXnAddr asks it.
uint32_t ExternalAddress();

}  // namespace band3::port_mapping
