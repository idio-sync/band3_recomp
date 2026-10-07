#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

// The Rock Band Stage Kit's commands, as StageKit::SetState(left, right) sends
// them: `right` says what to set and `left` is the colour's mask of eight LEDs.
// What a kit shows follows from the commands it was sent (StageKit), so a kit
// can be brought to any state with a few commands (KitSync). The same two
// bytes reach a Santroller kit in a HID output report and an Xbox 360 kit as
// rumble, and the Pico W bridges in RB3E's UDP Stage Kit event.
namespace band3::lights {

// right-byte commands
inline constexpr uint8_t kFogOn = 0x01;
inline constexpr uint8_t kFogOff = 0x02;
inline constexpr uint8_t kStrobeSlow = 0x03;  // speed 1; 0x04-0x06 are 2-4
inline constexpr uint8_t kStrobeFastest = 0x06;
inline constexpr uint8_t kStrobeOff = 0x07;
inline constexpr uint8_t kBlue = 0x20;
inline constexpr uint8_t kGreen = 0x40;
inline constexpr uint8_t kYellow = 0x60;
inline constexpr uint8_t kRed = 0x80;
inline constexpr uint8_t kAllOff = 0xFF;

struct Command {
    uint8_t left = 0;
    uint8_t right = 0;
    bool operator==(const Command&) const = default;
};

inline constexpr Command kAllOffCommand{0x00, kAllOff};

// The Stage Kit's lights: which of each colour's eight LEDs are lit, the
// strobe's speed and the fog machine.
struct StageKit {
    uint8_t red = 0, yellow = 0, green = 0, blue = 0;
    uint8_t strobe = 0;  // 0 off, 1-4
    bool fog = false;
    bool operator==(const StageKit&) const = default;
};

// one command applied, as RB3E's protocol reads it; a command the Stage Kit
// doesn't know changes nothing
StageKit ApplyStageKit(StageKit state, uint8_t left, uint8_t right);
// in one word, so the game thread can update it with a compare-and-swap
uint64_t PackStageKit(const StageKit& state);
StageKit UnpackStageKit(uint64_t packed);

// Brings one kit to the lights it should show, a command at a time: the fog,
// the strobe, then red, yellow, green and blue, each only when it differs.
// A kit whose lights aren't known (just found) is sent all-off first. Sending
// only differences means a backlog collapses to the latest lights and a
// command is never sent twice in a row.
struct KitSync {
    std::optional<StageKit> shown;
    StageKit wanted;

    // the command to send next; nullopt once the kit shows `wanted`
    std::optional<Command> Next() const;
    // `command` reached the kit
    void Sent(Command command);
};

// A Santroller device (pid.codes' 0x1209:0x2882) says what it is in its
// release number's high byte; 9 is a Stage Kit, as is XInput subtype 9.
inline constexpr uint16_t kSantrollerVendor = 0x1209;
inline constexpr uint16_t kSantrollerProduct = 0x2882;
inline constexpr uint8_t kSubtypeStageKit = 9;

bool IsSantrollerStageKit(uint16_t vendor, uint16_t product, uint16_t release);
// SDL's joystick GUID (32 hex digits) of a Santroller Stage Kit, or of an
// XInput device of subtype 9
bool IsStageKitGuid(std::string_view guid);

// the output report a Santroller Stage Kit in HID mode takes: report id 1,
// Santroller's command 0x5A, then left and right
std::array<uint8_t, 4> HidReport(Command command);

struct Rumble {
    uint16_t left = 0;
    uint16_t right = 0;
    bool operator==(const Rumble&) const = default;
};
// the rumble an Xbox 360 Stage Kit reads the command from, as the game sends it
Rumble XInputRumble(Command command);

// The launcher's test controls, as the RB3E Dashboard has them: LED masks...
inline constexpr uint8_t kPatternAll = 0xFF;
inline constexpr uint8_t kPatternNone = 0x00;
inline constexpr uint8_t kPatternOdds = 0x55;
inline constexpr uint8_t kPatternEvens = 0xAA;
inline constexpr uint8_t kPatternLeft = 0x0F;
inline constexpr uint8_t kPatternRight = 0xF0;

// ...and its scenes: a command, then how long to wait before the next
struct Step {
    Command command;
    int delay_ms = 0;
};
// each colour in turn, then off: the Test lights button
std::span<const Step> ColourCheck();
// odd and even LEDs alternating in red, green and blue, then off
std::span<const Step> Chase();

}  // namespace band3::lights
