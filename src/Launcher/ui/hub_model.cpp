#include "hub_model.h"
#include <algorithm>
#include "src/History/score_history.h"
#include "src/Input/instruments.h"

namespace band3::launcher::ui {

namespace {

uint32_t U32(std::span<const uint8_t> bytes, size_t at) {
    return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 |
           uint32_t(bytes[at + 3]) << 24;
}

}

std::vector<HubEntry> HubMenu(bool game_found) {
    std::vector<HubEntry> out;
    if (!game_found) out.push_back({HubItem::kFindGame});
    out.push_back({HubItem::kPlay, game_found});
    for (const HubItem item : {HubItem::kSettings, HubItem::kMusicVideos, HubItem::kTestControllers,
                               HubItem::kQuit}) {
        out.push_back({item});
    }
    return out;
}

std::string_view HubItemName(HubItem item) {
    switch (item) {
    case HubItem::kFindGame: return "Find Rock Band 3";
    case HubItem::kPlay: return "Play";
    case HubItem::kSettings: return "Settings";
    case HubItem::kMusicVideos: return "Music Videos";
    case HubItem::kTestControllers: return "Test Controllers";
    case HubItem::kQuit: return "Quit";
    }
    return "";
}

std::optional<std::string> DeluxeVersion(std::span<const uint8_t> dtb) {
    // the key's symbol, then a string node: its type (0x12), length and text
    constexpr std::string_view kKey = "rb3dx_version";
    const auto key = std::search(dtb.begin(), dtb.end(), kKey.begin(), kKey.end());
    if (key == dtb.end()) return std::nullopt;
    const size_t at = size_t(key - dtb.begin()) + kKey.size();
    if (at + 8 > dtb.size() || U32(dtb, at) != 0x12) return std::nullopt;
    const uint32_t length = U32(dtb, at + 4);
    if (length == 0 || at + 8 + length > dtb.size()) return std::nullopt;
    std::string version(reinterpret_cast<const char*>(dtb.data() + at + 8), length);
    // the revision, without the commit after it
    version = version.substr(0, version.find('+'));
    if (version.empty()) return std::nullopt;
    return version;
}

std::optional<Played> LastPlayed(std::string_view tail) {
    while (!tail.empty()) {
        const size_t newline = tail.find_last_of('\n', tail.size() - 1);
        const std::string_view line =
            newline == std::string_view::npos ? tail : tail.substr(newline + 1);
        if (const auto run = history::ParseLine(line)) return Played{run->title, run->artist};
        if (newline == std::string_view::npos) break;
        tail = tail.substr(0, newline);
    }
    return std::nullopt;
}

std::optional<IconCell> IconFor(uint8_t sub_type) {
    switch (sub_type) {
    case input::kSubtypeGuitar:
    case input::kSubtypeGuitarAlternate: return IconCell{1, 0};
    case input::kSubtypeGuitarBass: return IconCell{0, 0};
    case input::kSubtypeDrums: return IconCell{2, 0};
    case input::kSubtypeKeytar: return IconCell{3, 0};
    case input::kSubtypeProGuitar: return IconCell{1, 1};
    default: return std::nullopt;
    }
}

std::string ShortPath(std::string_view path, float max, const std::function<float(std::string_view)>& width) {
    if (width(path) <= max) return std::string(path);
    const std::string ellipsis = "\xE2\x80\xA6";
    const char sep = path.find('\\') != std::string_view::npos ? '\\' : '/';
    // the drive or root, then the folders
    const size_t first = path.find(sep);
    const std::string head(path.substr(0, first == std::string_view::npos ? 0 : first + 1));
    std::vector<std::string_view> parts;
    for (size_t at = head.size(); at < path.size();) {
        size_t end = path.find(sep, at);
        if (end == std::string_view::npos) end = path.size();
        if (end > at) parts.push_back(path.substr(at, end - at));
        at = end + 1;
    }
    // as many last folders as fit
    for (size_t keep = parts.size() > 1 ? parts.size() - 1 : 0; keep >= 1; keep--) {
        std::string candidate = head + ellipsis;
        for (size_t i = parts.size() - keep; i < parts.size(); i++) candidate += sep + std::string(parts[i]);
        if (width(candidate) <= max) return candidate;
    }
    // the last folder's end alone
    const std::string_view last = parts.empty() ? path : parts.back();
    for (size_t cut = 0; cut < last.size(); cut++) {
        // not inside a UTF-8 sequence
        if ((static_cast<unsigned char>(last[cut]) & 0xC0) == 0x80) continue;
        std::string candidate = ellipsis + std::string(last.substr(cut));
        if (width(candidate) <= max) return candidate;
    }
    return ellipsis;
}

}
