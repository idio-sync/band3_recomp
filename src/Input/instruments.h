#pragma once
#include <array>
#include <cstddef>
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

// XInput subtypes. RB3 sets up the instrument ones; anything else, or a pad
// whose capabilities it can't read, it takes for a gamepad (kJoypadAnalog).
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

// What a guitar or drum kit is pressing, read back from what it reports: the
// encoders above in reverse, for the launcher's test view. Every source
// reaches it in this form, the HID and MIDI drivers' encoded instruments and
// real Xbox 360 instruments alike. `nav` holds every menu button as the game
// reads it, including those the frets, strums or pads also press (a green fret
// is A).
GuitarInputs DecodeGuitar(const Gamepad360& g);

// RB2-or-later kits by the capabilities, as RB3 tells them apart (IsRb2Drums):
// pads and cymbals from the face buttons with the pad and cymbal flags and the
// d-pad markers, and velocities from the sticks. Where the report can't tell
// hits apart, the common case is taken: with both flags set, green is a cymbal
// unless yellow or blue's cymbal marker explains the cymbal flag, and a pad
// flag nothing else explains is a pad of the cymbal's color, whose velocity is
// then in red's axis (as EncodeDrums puts it). Face buttons without a flag are
// menu presses only. An RB1 kit has neither flags nor velocity: each face
// button is its pad, hit at 127, and the d-pad only navigates.
DrumInputs DecodeDrums(const Gamepad360& g, const Caps360& caps);

// whether RB3 reads a drum kit with these capabilities as RB2-or-later
// (cymbals and velocity) rather than RB1
bool IsRb2Drums(const Caps360& caps);

// RB3 keytar
Caps360 KeysCaps();
Gamepad360 EncodeKeys(const KeysInputs& in);

// What a keytar is pressing, read back from what it reports (EncodeKeys in
// reverse), for the launcher's test view: the held keys from the triggers and
// sThumbLX's bits, their velocities paired with them from the lowest key up,
// overdrive and the menu buttons. The report carries five velocities, so a
// sixth key held and up reads at 127, as does a held key reported at 0.
KeysInputs DecodeKeys(const Gamepad360& g);

// Mustang (buttons, 17 frets) or Squier (strings, 22 frets)
Caps360 ProGuitarCaps(ProGuitarModel model);
Gamepad360 EncodeProGuitar(const ProGuitarInputs& in);

// The 16 bytes RB3 reads Pro Keys and Pro Guitar from: JoypadData's
// mProGuitarData, which UsbMidiKeyboard::Poll reads as ProKeysData and
// UsbMidiGuitar::Poll as ProGuitarData (rb3-xenon, src/system/os). They are an
// Xbox 360 instrument's report from the left trigger on, in its little-endian
// wire order, which the XInput state holds the first ten bytes of; the rest
// (accelerometer, hand placement, connected accessories) stay zero. RB3 gets
// them from XamInputRawState, which ReXGlue doesn't implement, so band3 writes
// them itself (Hooks/pro_instruments.cpp).
inline constexpr size_t kProDataSize = 16;
using ProData = std::array<uint8_t, kProDataSize>;
ProData EncodeProData(const Gamepad360& g);

}
