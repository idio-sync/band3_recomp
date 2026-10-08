#pragma once
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Microphones on this PC, sung into as Xbox 360 USB microphones.
//
// RB3 runs one guest thread per mic slot (ExternalMic::sampleProcessThread),
// which reads the XMic library; XMic needs the MicDeviceRequest kernel import,
// which ReXGlue doesn't implement. band3 replaces that thread
// (Hooks/usb_mic.cpp) with a loop that connects the slot and hands the game
// 48 kHz mono 16-bit big-endian PCM through ExternalMicClientMgr::AddAudio,
// as ExternalMic::dataReady does. usb_mic_capture.h records it with SDL.
//
// Time is passed in rather than read, so this is testable.

namespace band3::audio::usb_mic {

using Clock = std::chrono::steady_clock;

// MicXbox::GetSampleRate (vtable +124, 0x827297B0, folded with MicNull's),
// the rate GameMic gives its PitchDetector. rb3-xenon's MicXbox has 16000, which
// the retail image doesn't: fed 16 kHz, every note read a fifth sharp.
constexpr int kSampleRate = 48000;
// ExternalMic::Init makes four
constexpr int kSlots = 4;
// but vocals have at most three singers, so only the first three are recorded
// into (usb_mic_devices) and offered in the settings
constexpr int kSingers = 3;
static_assert(kSingers <= kSlots);
// ExternalMic::dataReady's buffer, the most it hands AddAudio at once
constexpr size_t kMaxChunk = 2048;
// the most audio kept waiting for the game; older audio is dropped so the
// pitch arrow doesn't fall behind the singer
constexpr std::chrono::milliseconds kMaxBacklog{250};
constexpr size_t kMaxBacklogBytes = kSampleRate * 2 * kMaxBacklog.count() / 1000;

// The game sets a mic's gain from 0 to 1 (MicXbox::SetGain), and
// ExternalMic::processGain turns it into the microphone's own gain, between the
// lowest and highest the device reports (XMicGetGain, a ratio). A PC
// microphone has no such control here, so band3 scales the samples instead,
// over the range of the RB microphones the game knows, and pinned so the gain
// ProfileMgr::UpdateMicLevels gives a generic USB mic plays at the level the PC
// records.
//
// ratio_sliders in sound.dta, HX_XBOX generic_usb
constexpr float kNormalGain = 0.225f;
// min_gain of rb2_logitech_usb in mic_types (default.dta), about -6 dB;
// TambourineManager turns the gain to 0 during tambourine sections
constexpr float kMinGainRatio = 0.501187f;

// the ratio to scale samples by for the game's gain
float GainRatio(float gain);

// scales big-endian samples by `ratio`, clipping at full scale
void ApplyGain(std::span<uint8_t> pcm, float ratio);

// usb_mic_devices: names, or parts of names, comma separated, in slot order.
// Blank entries are kept, so ",Yeti" leaves slot 1 empty.
std::vector<std::string> ParseDeviceList(std::string_view list);

// case-insensitive: `wanted` appears in `device`
bool NameMatches(std::string_view device, std::string_view wanted);

// A sine at a fixed pitch, as the game's PCM, for testing without a
// microphone. Each Read produces the samples owed since the last, so the rate
// holds however the loop is timed.
class ToneSource {
public:
    ToneSource(double hz, Clock::time_point start, int16_t amplitude = 8000);

    // Writes the big-endian samples owed at `now`, up to out.size() / 2, and
    // returns the bytes written. More than kMaxBacklog owed is skipped.
    size_t Read(std::span<uint8_t> out, Clock::time_point now);

private:
    double step_;
    double phase_ = 0;
    int16_t amplitude_;
    Clock::time_point start_;
    uint64_t produced_ = 0;
};

// what a slot's loop does next
enum class Action {
    kWait,        // nothing is feeding it, or the game said no a moment ago
    kConnect,     // call ExternalMicClientProxy::OnMicConnected
    kFeed,        // hand the game whatever audio is ready
    kDisconnect,  // call ExternalMicClientMgr::OnMicDisconnected
};

// When a slot connects, feeds and disconnects. The game refuses the connection
// (OnMicConnected returns an error) until it has attached a MicXbox to the
// slot, so a refused connection is retried every kConnectRetry.
class Slot {
public:
    static constexpr std::chrono::milliseconds kConnectRetry{500};

    // `source_ready`: a microphone (or the test tone) is feeding this slot
    Action Next(bool source_ready, Clock::time_point now) const;

    // the game's answer to OnMicConnected
    void Connected(bool accepted, Clock::time_point now);
    void Disconnected();

    bool connected() const { return connected_; }

private:
    bool connected_ = false;
    Clock::time_point retry_{};
};

}
