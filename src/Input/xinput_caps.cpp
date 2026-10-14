#include "xinput_caps.h"
#include <array>

namespace band3::input {

namespace {

// SDL_CreateJoystickGUID's driver signature for its XInput backend
constexpr uint8_t kXInputSignature = 'x';

std::optional<std::array<uint8_t, 16>> GuidBytes(std::string_view guid) {
    if (guid.size() != 32) return std::nullopt;
    std::array<uint8_t, 16> bytes{};
    for (size_t i = 0; i < guid.size(); i++) {
        const char c = guid[i];
        uint8_t nibble = 0;
        if (c >= '0' && c <= '9') nibble = c - '0';
        else if (c >= 'a' && c <= 'f') nibble = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') nibble = c - 'A' + 10;
        else return std::nullopt;
        bytes[i / 2] = static_cast<uint8_t>(bytes[i / 2] << 4 | nibble);
    }
    return bytes;
}

uint16_t Le16(const std::array<uint8_t, 16>& bytes, size_t at) {
    return static_cast<uint16_t>(bytes[at] | bytes[at + 1] << 8);
}

// the pad could be the device SDL reads as `ids`
bool CouldBe(const XInputGuidIds& ids, const WindowsPad& pad) {
    if (pad.caps.sub_type != ids.subtype) return false;
    if (ids.vendor == 0 || pad.vendor == 0) return true;
    return pad.vendor == ids.vendor && pad.product == ids.product;
}

}

std::optional<XInputGuidIds> XInputIdsFromGuid(std::string_view guid) {
    const auto bytes = GuidBytes(guid);
    if (!bytes || (*bytes)[14] != kXInputSignature) return std::nullopt;
    XInputGuidIds ids;
    ids.subtype = (*bytes)[15];
    // with the ids, bytes 4-13 are vendor, 0, product, 0, version (each 16-bit
    // little-endian); without, SDL fills them with the device's name
    const bool has_ids = Le16(*bytes, 4) != 0 && Le16(*bytes, 6) == 0 && Le16(*bytes, 10) == 0;
    if (has_ids) {
        ids.vendor = Le16(*bytes, 4);
        ids.product = Le16(*bytes, 8);
    }
    return ids;
}

std::optional<Caps360> MatchWindowsCaps(const XInputGuidIds& ids,
                                        std::span<const WindowsPad> pads) {
    std::optional<Caps360> found;
    for (const WindowsPad& pad : pads) {
        if (!CouldBe(ids, pad)) continue;
        if (found && !(*found == pad.caps)) return std::nullopt;
        found = pad.caps;
    }
    return found;
}

}
