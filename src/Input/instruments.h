#pragma once
#include <array>
#include <cstdint>

// Rock Band instruments in the form an Xbox 360 reports them, which is the only
// form RB3 reads. Anything that drives an instrument (the Instrument Lab's
// virtual one now, PS3/Wii/PS4 adapters later) fills in the plain input structs
// below and encodes them here, so every source shares one encoding.
//
// Layouts follow PlasticBand's Xbox 360 docs
// (github.com/TheNathannator/PlasticBand, Docs/Instruments/*/Xbox 360.md); the
// capability values follow how RB3 tells models apart in ReadSingleXinputJoypad
// and the SetupHX* functions (rb3-xenon, src/system/os/Joypad_Xinput.cpp and
// Joypad_Xbox.cpp).

namespace band3::input {

// XInput subtypes. RB3 only accepts the instrument ones; anything else
// (a gamepad included) reads as no controller.
inline constexpr uint8_t kSubtypeGamepad = 1;
inline constexpr uint8_t kSubtypeGuitar = 6;
inline constexpr uint8_t kSubtypeGuitarAlternate = 7;
inline constexpr uint8_t kSubtypeDrums = 8;
inline constexpr uint8_t kSubtypeGuitarBass = 11;
inline constexpr uint8_t kSubtypeKeytar = 15;
inline constexpr uint8_t kSubtypeProGuitar = 25;

// whether RB3 treats a device reporting this subtype as an instrument
bool IsRb3InstrumentSubtype(uint8_t subtype);

// XINPUT_GAMEPAD button bits
namespace xbox {
inline constexpr uint16_t kDpadUp = 0x0001;
inline constexpr uint16_t kDpadDown = 0x0002;
inline constexpr uint16_t kDpadLeft = 0x0004;
inline constexpr uint16_t kDpadRight = 0x0008;
inline constexpr uint16_t kStart = 0x0010;
inline constexpr uint16_t kBack = 0x0020;
inline constexpr uint16_t kLeftThumb = 0x0040;
inline constexpr uint16_t kRightThumb = 0x0080;
inline constexpr uint16_t kLeftShoulder = 0x0100;
inline constexpr uint16_t kRightShoulder = 0x0200;
inline constexpr uint16_t kButtonA = 0x1000;
inline constexpr uint16_t kButtonB = 0x2000;
inline constexpr uint16_t kButtonX = 0x4000;
inline constexpr uint16_t kButtonY = 0x8000;
}

// XINPUT_GAMEPAD in host byte order
struct Gamepad360 {
    uint16_t buttons = 0;
    uint8_t left_trigger = 0;
    uint8_t right_trigger = 0;
    int16_t thumb_lx = 0;
    int16_t thumb_ly = 0;
    int16_t thumb_rx = 0;
    int16_t thumb_ry = 0;
};

// the XINPUT_CAPABILITIES fields RB3 looks at
struct Caps360 {
    uint8_t sub_type = kSubtypeGamepad;
    uint16_t flags = 0;
    Gamepad360 gamepad;
};

// buttons every instrument has
struct NavInputs {
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool start = false;
    bool back = false;
    bool dpad_up = false;
    bool dpad_down = false;
    bool dpad_left = false;
    bool dpad_right = false;
};

enum Fret { kGreen, kRed, kYellow, kBlue, kOrange, kFretCount };

struct GuitarInputs {
    NavInputs nav;
    std::array<bool, kFretCount> frets{};
    // the held frets are the lower (solo) ones
    bool solo = false;
    bool strum_up = false;
    bool strum_down = false;
    float whammy = 0.0f;  // 0 = released, 1 = fully pressed
    // 0 = level, 1 = straight up; RB3 decides what counts as tilted
    float tilt = 0.0f;
    uint8_t pickup = 0;
};

enum DrumPad { kRedPad, kYellowPad, kBluePad, kGreenPad, kPadCount };
enum Cymbal { kYellowCymbal, kBlueCymbal, kGreenCymbal, kCymbalCount };

// velocities are 1 (softest) to 127 (hardest); 0 means not hit
struct DrumInputs {
    NavInputs nav;
    std::array<uint8_t, kPadCount> pads{};
    std::array<uint8_t, kCymbalCount> cymbals{};
    bool kick1 = false;
    bool kick2 = false;
};

inline constexpr int kKeyCount = 25;  // C1 to C3

// velocities are 1 (softest) to 127 (hardest); 0 means the key is up
struct KeysInputs {
    NavInputs nav;
    std::array<uint8_t, kKeyCount> keys{};
    bool overdrive = false;
};

enum GuitarString {
    kStringLowE, kStringA, kStringD, kStringG, kStringB, kStringHighE, kStringCount
};
inline constexpr int kMaxProFret = 22;

struct ProGuitarInputs {
    NavInputs nav;
    // fret held on each string, 0 = open
    std::array<uint8_t, kStringCount> frets{};
    // how hard each string is sounding, 1 to 127; 0 = silent
    std::array<uint8_t, kStringCount> velocities{};
    // the 5-fret color each string's fret maps to, for 5-fret parts
    std::array<bool, kFretCount> colors{};
    bool solo = false;
};

enum class ProGuitarModel { kMustang, kSquier };

// an RB2-or-later wireless guitar, or with rb2 = false an RB1-style guitar
// (no auto-calibration sensors)
Caps360 GuitarCaps(bool rb2 = true);
Gamepad360 EncodeGuitar(const GuitarInputs& in);

// an RB2-or-later drum kit, so cymbals and velocities are read, or with
// rb2 = false an RB1 kit (no pad/cymbal flags, no velocity)
Caps360 DrumCaps(bool rb2 = true);
Gamepad360 EncodeDrums(const DrumInputs& in);

// RB3 keytar
Caps360 KeysCaps();
Gamepad360 EncodeKeys(const KeysInputs& in);

// Mustang (buttons, 17 frets) or Squier (strings, 22 frets)
Caps360 ProGuitarCaps(ProGuitarModel model);
Gamepad360 EncodeProGuitar(const ProGuitarInputs& in);

}
