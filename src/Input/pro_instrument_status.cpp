#include "pro_instrument_status.h"
#include <atomic>
#include <mutex>

namespace band3::input {

namespace {

std::mutex g_mutex;
std::array<ProPadStatus, kProPads> g_pads{};
std::atomic<bool> g_polled{false};

}

void RecordProPad(int pad, bool connected, uint8_t subtype, uint32_t game_type,
                  const ProData* written) {
    if (pad < 0 || pad >= kProPads) return;
    g_polled = true;
    std::lock_guard<std::mutex> lock(g_mutex);
    ProPadStatus& s = g_pads[pad];
    s.connected = connected;
    s.subtype = subtype;
    s.game_type = game_type;
    s.writing = written != nullptr;
    if (written) {
        s.data = *written;
        s.writes++;
    }
}

bool ProPadsPolled() { return g_polled; }

std::array<ProPadStatus, kProPads> ProPadStatuses() {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_pads;
}

const char* JoypadTypeName(uint32_t type) {
    switch (type) {
    case 0: return "none";
    case 5: return "Xbox guitar";
    case 6: return "Xbox RB2 guitar";
    case 7: return "Xbox Rock Revolution guitar";
    case 8: return "Xbox drums";
    case 9: return "Xbox RB2 drums";
    case 10: return "Xbox Rock Revolution drums";
    case 11: return "Xbox Stage Kit";
    case 29: return "Xbox core guitar";
    case 30: return "Xbox pro guitar (Mustang)";
    case 31: return "Xbox pro guitar (Squier, 22 frets)";
    case 32: return "Xbox MIDI Pro Adapter keyboard";
    case 33: return "Xbox MIDI Pro Adapter drums";
    case 34: return "Xbox keytar";
    default: return nullptr;
    }
}

}
