#include "stagekit.h"
#include "src/Input/guitar_type.h"

namespace band3::lights {

namespace {

// a byte of a GUID's hex digits, from the `index`th byte
std::optional<uint8_t> GuidByte(std::string_view guid, size_t index) {
    uint8_t value = 0;
    for (const char c : guid.substr(index * 2, 2)) {
        value <<= 4;
        if (c >= '0' && c <= '9') value |= c - '0';
        else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
        else return std::nullopt;
    }
    return value;
}

// a little-endian word of a GUID, from the `index`th byte
std::optional<uint16_t> GuidWord(std::string_view guid, size_t index) {
    const auto low = GuidByte(guid, index);
    const auto high = GuidByte(guid, index + 1);
    if (!low || !high) return std::nullopt;
    return static_cast<uint16_t>(*low | *high << 8);
}

constexpr Step kColourCheck[] = {
    {{kPatternAll, kRed}, 600},    {{kPatternAll, kGreen}, 600}, {{kPatternAll, kBlue}, 600},
    {{kPatternAll, kYellow}, 600}, {kAllOffCommand, 0},
};

constexpr Step kChase[] = {
    {{kPatternOdds, kRed}, 250},   {{kPatternEvens, kRed}, 250},
    {{kPatternOdds, kGreen}, 250}, {{kPatternEvens, kGreen}, 250},
    {{kPatternOdds, kBlue}, 250},  {{kPatternEvens, kBlue}, 250},
    {kAllOffCommand, 0},
};

}

StageKit ApplyStageKit(StageKit state, uint8_t left, uint8_t right) {
    switch (right) {
    case kBlue: state.blue = left; break;
    case kGreen: state.green = left; break;
    case kYellow: state.yellow = left; break;
    case kRed: state.red = left; break;
    case kFogOn: state.fog = true; break;
    case kFogOff: state.fog = false; break;
    case 0x03:
    case 0x04:
    case 0x05:
    case 0x06: state.strobe = static_cast<uint8_t>(right - 0x02); break;
    case kStrobeOff: state.strobe = 0; break;
    case kAllOff: state = {}; break;
    default: break;  // not a command the Stage Kit knows
    }
    return state;
}

uint64_t PackStageKit(const StageKit& state) {
    return uint64_t(state.red) | uint64_t(state.yellow) << 8 | uint64_t(state.green) << 16 |
           uint64_t(state.blue) << 24 | uint64_t(state.strobe) << 32 |
           uint64_t(state.fog ? 1 : 0) << 40;
}

StageKit UnpackStageKit(uint64_t packed) {
    StageKit state;
    state.red = static_cast<uint8_t>(packed);
    state.yellow = static_cast<uint8_t>(packed >> 8);
    state.green = static_cast<uint8_t>(packed >> 16);
    state.blue = static_cast<uint8_t>(packed >> 24);
    state.strobe = static_cast<uint8_t>(packed >> 32);
    state.fog = ((packed >> 40) & 1) != 0;
    return state;
}

std::optional<Command> KitSync::Next() const {
    if (!shown) return kAllOffCommand;
    const StageKit& now = *shown;
    if (now.fog != wanted.fog) return Command{0, wanted.fog ? kFogOn : kFogOff};
    if (now.strobe != wanted.strobe) {
        return Command{0, wanted.strobe ? static_cast<uint8_t>(0x02 + wanted.strobe) : kStrobeOff};
    }
    if (now.red != wanted.red) return Command{wanted.red, kRed};
    if (now.yellow != wanted.yellow) return Command{wanted.yellow, kYellow};
    if (now.green != wanted.green) return Command{wanted.green, kGreen};
    if (now.blue != wanted.blue) return Command{wanted.blue, kBlue};
    return std::nullopt;
}

void KitSync::Sent(Command command) {
    shown = ApplyStageKit(shown.value_or(StageKit{}), command.left, command.right);
}

bool IsSantrollerStageKit(uint16_t vendor, uint16_t product, uint16_t release) {
    return vendor == kSantrollerVendor && product == kSantrollerProduct &&
           release >> 8 == kSubtypeStageKit;
}

bool IsStageKitGuid(std::string_view guid) {
    if (guid.size() != 32) return false;
    if (input::XInputSubtypeFromGuid(guid) == kSubtypeStageKit) return true;
    // SDL_CreateJoystickGUID: bus, CRC, vendor, 0, product, 0, version, driver
    const auto vendor = GuidWord(guid, 4);
    const auto product = GuidWord(guid, 8);
    const auto version = GuidWord(guid, 12);
    return vendor && product && version && IsSantrollerStageKit(*vendor, *product, *version);
}

bool IsStageKitDevice(uint8_t subtype, std::string_view guid) {
    return subtype == kSubtypeStageKit || IsStageKitGuid(guid);
}

std::array<uint8_t, 4> HidReport(Command command) {
    return {0x01, 0x5A, command.left, command.right};
}

Rumble XInputRumble(Command command) {
    return {static_cast<uint16_t>(command.left << 8 | 0xFF),
            static_cast<uint16_t>(command.right << 8 | 0xFF)};
}

std::span<const Step> ColourCheck() { return kColourCheck; }
std::span<const Step> Chase() { return kChase; }

}  // namespace band3::lights
