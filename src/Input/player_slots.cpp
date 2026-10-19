#include "player_slots.h"
#include <cctype>

namespace band3::input {

std::array<std::vector<size_t>, kPlayers> AssignPlayers(
    const std::vector<SlotDevice>& devices, const std::array<bool, kPlayers>& reserved) {
    std::array<std::vector<size_t>, kPlayers> players;
    std::array<bool, kPlayers> taken = reserved;

    // the pads that keep their seat; the rest wait for the free players
    std::vector<size_t> unseated;
    for (size_t i = 0; i < devices.size(); i++) {
        const SlotDevice& device = devices[i];
        switch (device.kind) {
        case SlotDevice::Kind::kVirtual:
            if (device.virtual_player >= 1 && device.virtual_player <= kPlayers) {
                players[device.virtual_player - 1].push_back(i);
            }
            break;
        case SlotDevice::Kind::kSkipped:
            break;
        case SlotDevice::Kind::kSynthetic:
            // a virtual instrument on player 1 has the slot to itself, so a type
            // change empties it and RB3 reads the new type when it comes back
            if (!reserved[0]) players[0].push_back(i);
            break;
        case SlotDevice::Kind::kPad: {
            const int seat = device.seat - 1;
            if (seat >= 0 && seat < kPlayers && !taken[seat]) {
                players[seat].push_back(i);
                taken[seat] = true;
            } else {
                unseated.push_back(i);
            }
            break;
        }
        }
    }

    for (size_t i : unseated) {
        for (int p = 0; p < kPlayers; p++) {
            if (taken[p]) continue;
            players[p].push_back(i);
            taken[p] = true;
            break;
        }
    }
    return players;
}

int ProbePlayer(const std::array<bool, kPlayers>& feeds) {
    for (int player = kPlayers - 1; player >= 0; player--) {
        if (!feeds[player]) return player;
    }
    // a device feeds one player at most; this is only for completeness
    return kPlayers - 1;
}

namespace {

std::string_view Trim(std::string_view s) {
    const size_t first = s.find_first_not_of(" \t");
    if (first == std::string_view::npos) return {};
    return s.substr(first, s.find_last_not_of(" \t") - first + 1);
}

bool SameKey(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) {
            return false;
        }
    }
    return true;
}

// the list's entries, trimmed, without empty ones
std::vector<std::string_view> Entries(std::string_view list) {
    std::vector<std::string_view> out;
    while (!list.empty()) {
        const size_t end = list.find(';');
        const std::string_view entry = Trim(list.substr(0, end));
        if (!entry.empty()) out.push_back(entry);
        if (end == std::string_view::npos) break;
        list.remove_prefix(end + 1);
    }
    return out;
}

}

std::vector<std::string> LeftOutKeys(std::string_view list) {
    std::vector<std::string> out;
    for (const std::string_view entry : Entries(list)) out.emplace_back(entry);
    return out;
}

std::string_view LeftOutName(std::string_view key) { return Trim(key.substr(0, key.rfind('|'))); }

bool IsSteamDeckControls(std::string_view name, uint16_t vendor, uint16_t product) {
    // Valve's, the LCD and OLED Decks' alike
    constexpr uint16_t kValve = 0x28de, kDeck = 0x1205;
    if (vendor == kValve && product == kDeck) return true;
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return lower.find("steam deck") != std::string::npos;
}

std::string LeftOutKey(std::string_view name, std::string_view guid) {
    // the list's separators can't be in a name
    std::string key;
    for (const char c : Trim(name)) key += c == ';' || c == '|' ? ' ' : c;
    key += '|';
    key += guid;
    return key;
}

bool IsLeftOut(std::string_view list, std::string_view key) {
    for (const std::string_view entry : Entries(list)) {
        if (SameKey(entry, key)) return true;
    }
    return false;
}

std::string ToggleLeftOut(std::string_view list, std::string_view key) {
    std::string out;
    bool removed = false;
    for (const std::string_view entry : Entries(list)) {
        if (SameKey(entry, key)) {
            removed = true;
            continue;
        }
        if (!out.empty()) out += ';';
        out += entry;
    }
    if (!removed) {
        if (!out.empty()) out += ';';
        out += key;
    }
    return out;
}

}
