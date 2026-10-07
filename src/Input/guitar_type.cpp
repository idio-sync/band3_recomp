#include "guitar_type.h"
#include "instruments.h"

namespace band3::input {

namespace {

// SDL_CreateJoystickGUID's driver signature for its XInput backend
constexpr uint8_t kXInputSignature = 'x';

std::optional<uint8_t> HexByte(std::string_view text) {
    uint8_t value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= c - '0';
        else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
        else return std::nullopt;
    }
    return value;
}

}

std::optional<GuitarType> ParseGuitarType(std::string_view value) {
    if (value == "auto") return GuitarType::kAuto;
    if (value == "rock_band") return GuitarType::kRockBand;
    if (value == "guitar_hero") return GuitarType::kGuitarHero;
    return std::nullopt;
}

bool IsGuitarSubtype(uint8_t subtype) {
    return subtype == kSubtypeGuitar || subtype == kSubtypeGuitarAlternate ||
           subtype == kSubtypeGuitarBass;
}

std::optional<uint8_t> XInputSubtypeFromGuid(std::string_view guid) {
    // 16 bytes, two hex digits each
    if (guid.size() != 32) return std::nullopt;
    for (size_t i = 0; i < guid.size(); i += 2) {
        if (!HexByte(guid.substr(i, 2))) return std::nullopt;
    }
    if (HexByte(guid.substr(28, 2)) != kXInputSignature) return std::nullopt;
    return HexByte(guid.substr(30, 2));
}

uint8_t GuitarSubtypeFor(GuitarType type, uint8_t reported, std::optional<uint8_t> native) {
    if (!IsGuitarSubtype(reported)) return reported;
    switch (type) {
    case GuitarType::kGuitarHero: return kSubtypeGuitarAlternate;
    case GuitarType::kRockBand:
        return reported == kSubtypeGuitarAlternate ? kSubtypeGuitar : reported;
    case GuitarType::kAuto: break;
    }
    return native && IsGuitarSubtype(*native) ? *native : reported;
}

}
