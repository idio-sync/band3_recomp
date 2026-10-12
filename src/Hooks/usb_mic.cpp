#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <span>
#include <thread>
#include "generated/band3_init.h"
#include "src/Audio/usb_mic.h"
#include "src/Audio/usb_mic_capture.h"
#include "src/Game/Symbol.h"

// Replaces the thread RB3 runs for each mic slot while usb_mics is on (see
// Audio/usb_mic.h). The game's thread reads the XMic library; this one connects
// the slot when a microphone feeds it and hands the game its audio the way
// ExternalMic::dataReady does, at the gain the game asks for, as
// ExternalMic::processGain does. Structures and calls are from rb3-xenon's
// src/system/synth_xbox/ExternalMic.cpp and Mic.cpp.
//
// With usb_mic_monitor, band3 also plays each slot to the speakers itself
// (Audio/usb_mic_capture.h), and MicXbox::Poll is kept from playing it back.

extern "C" void __imp__ExternalMicThreadEntry(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MicXbox__AddData(PPCContext& ctx, uint8_t* base);
extern "C" void __imp__MicXbox__Poll(PPCContext& ctx, uint8_t* base);
REX_EXTERN(ExternalMicClientMgr__GetMasterForIndex);
REX_EXTERN(ExternalMicClientProxy__OnMicConnected);
REX_EXTERN(ExternalMicClientMgr__AddAudio);
REX_EXTERN(ExternalMicClientMgr__OnMicDisconnected);
REX_EXTERN(ExternalMicClientMgr__GetRequiredGain);

namespace {

using namespace band3::audio;
using namespace band3::audio::usb_mic;
using namespace std::chrono_literals;

// ExternalMic, the thread's argument
constexpr uint32_t kExternalMic_DeviceId = 0x4;
// set by ~ExternalMic, which then waits for the thread to end
constexpr uint32_t kExternalMic_Quit = 0x8;
// what ExternalMic::NumConnectedMics counts
constexpr uint32_t kExternalMic_Connected = 0x9;

// the name MicXbox gives a USB microphone until one connects
constexpr const char* kDeviceName = "generic_usb";

// Guest stack below this thread's frame: band3::Symbol builds the device name
// within 0x400 of r1, the audio buffer sits below that, and calls into the game
// get their frames below the buffer.
constexpr uint32_t kAudioBuffer = 0x1000 + kMaxChunk;
constexpr uint32_t kCallFrames = 0x2000;

// the game reads its mics once a frame; this keeps them topped up between
constexpr auto kPollInterval = 5ms;
// sooner while band3 plays the slot to the speakers, where it's heard
constexpr auto kMonitorPollInterval = 1ms;

// MicXbox (rb3-xenon's synth_xbox/Mic.h, checked against MicXbox::Poll at
// 0x82B5EA28): the voice it plays the mic back through, the first FxSend of
// its effects, its volume (a ratio) and mute
constexpr uint32_t kMicXbox_PlaybackVoice = 0x18;
constexpr uint32_t kMicXbox_FxSend = 0x9060;
constexpr uint32_t kMicXbox_Volume = 0x9064;
constexpr uint32_t kMicXbox_Mute = 0x9068;

// FxSend (FxSend::Load at 0x82716F60): the next send in the chain, its dry
// gain in dB, and its virtual UpdateMix, which applies the gains
constexpr uint32_t kFxSend_Next = 48;
constexpr uint32_t kFxSend_DryGain = 60;
constexpr uint32_t kFxSend_UpdateMix = 104;
// as FxSend has it for silence
constexpr float kSilentDb = -96.0f;

// The last send of the mics' effects (mic_fx.milo's delay.send), whose dry
// signal band3 turned off while it plays the mics itself, and what it was. The
// game keeps that send's echo and the compressor's reverb.
uint32_t g_dry_send = 0;
float g_dry_send_gain = 0.0f;

// the MicXbox each slot's audio goes to, learnt as Feed hands it over
std::array<std::atomic<uint32_t>, kSlots> g_slot_mic{};
// the slot this thread is feeding, while it calls AddAudio
thread_local int t_feeding_slot = -1;

int SlotForMic(uint32_t mic) {
    for (int i = 0; i < kSlots; i++) {
        if (g_slot_mic[i].load(std::memory_order_relaxed) == mic) return i;
    }
    return -1;
}

PPCContext CallContext(const PPCContext& ctx) {
    PPCContext call{};
    call.r1.u64 = ctx.r1.u32 - kCallFrames;
    call.r13 = ctx.r13;
    return call;
}

// ExternalMicClientMgr::GetMasterForIndex(dev)->OnMicConnected(dev, false, name).
// It fails until the game has attached a MicXbox to the slot.
bool Connect(PPCContext& ctx, uint8_t* base, uint32_t dev) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = dev;
    ExternalMicClientMgr__GetMasterForIndex(call, base);
    const uint32_t proxy = call.r3.u32;
    if (!proxy) return false;

    const band3::Symbol name(ctx, base, kDeviceName);
    if (!name.guest_addr()) return false;

    call = CallContext(ctx);
    call.r3.u64 = proxy;
    call.r4.u64 = dev;
    // MicXbox keeps this bool; it only picks the delay for playing the mic back
    call.r5.u64 = 0;
    call.r6.u64 = name.guest_addr();
    ExternalMicClientProxy__OnMicConnected(call, base);
    return call.r3.u32 == 0;
}

void Disconnect(PPCContext& ctx, uint8_t* base, uint32_t dev) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = dev;
    ExternalMicClientMgr__OnMicDisconnected(call, base);
}

// ExternalMicClientMgr::GetRequiredGain(dev), the slot's MicXbox gain (0-1)
float RequiredGain(PPCContext& ctx, uint8_t* base, uint32_t dev) {
    PPCContext call = CallContext(ctx);
    call.r3.u64 = dev;
    ExternalMicClientMgr__GetRequiredGain(call, base);
    return static_cast<float>(call.f1.f64);
}

// ExternalMicClientMgr::AddAudio(dev, buffer, length), for all the audio ready
void Feed(PPCContext& ctx, uint8_t* base, uint32_t dev, int slot, float ratio) {
    const uint32_t buffer = ctx.r1.u32 - kAudioBuffer;
    std::array<uint8_t, kMaxChunk> pcm;
    while (true) {
        const size_t length = ReadUsbMic(slot, pcm);
        if (length == 0) return;
        ApplyGain(std::span(pcm).first(length), ratio);
        MonitorUsbMic(slot, std::span(pcm).first(length));
        std::memcpy(REX_RAW_ADDR(buffer), pcm.data(), length);
        PPCContext call = CallContext(ctx);
        call.r3.u64 = dev;
        call.r4.u64 = buffer;
        call.r5.u64 = length;
        t_feeding_slot = slot;
        ExternalMicClientMgr__AddAudio(call, base);
        t_feeding_slot = -1;
        NoteUsbMicFed(slot, length);
        if (length < pcm.size()) return;
    }
}

}

// ExternalMicThreadEntry(ExternalMic*), which the ExternalMic constructor
// starts; in the game it only calls ExternalMic::sampleProcessThread
extern "C" REX_FUNC(ExternalMicThreadEntry)
{
    if (!UsbMicsRunning()) {
        __imp__ExternalMicThreadEntry(ctx, base);
        return;
    }

    const uint32_t mic = ctx.r3.u32;
    const uint32_t dev = REX_LOAD_U32(mic + kExternalMic_DeviceId);
    if (dev >= static_cast<uint32_t>(kSlots)) {
        REXLOG_WARN("USB mics: unexpected mic device {}; leaving it to the game", dev);
        __imp__ExternalMicThreadEntry(ctx, base);
        return;
    }
    const int slot = static_cast<int>(dev);
    REXLOG_DEBUG("USB mics: mic slot {} thread started", slot + 1);
    NoteUsbMicThread(slot);

    Slot state;
    bool reported_refusal = false;
    // processGain's mLastGain, so a change is logged once
    float last_gain = -1.0f;
    while (!REX_LOAD_U8(mic + kExternalMic_Quit)) {
        const auto now = Clock::now();
        switch (state.Next(UsbMicReady(slot), now)) {
        case Action::kConnect: {
            const bool accepted = Connect(ctx, base, dev);
            state.Connected(accepted, now);
            NoteUsbMicConnect(slot, accepted);
            if (accepted) {
                REX_STORE_U8(mic + kExternalMic_Connected, 1);
                REXLOG_INFO("USB mics: the game connected mic slot {}", slot + 1);
                reported_refusal = false;
            } else if (!reported_refusal) {
                REXLOG_DEBUG("USB mics: the game hasn't set up mic slot {} yet; retrying",
                             slot + 1);
                reported_refusal = true;
            }
            break;
        }
        case Action::kFeed: {
            const float gain = RequiredGain(ctx, base, dev);
            const float ratio = GainRatio(gain);
            if (gain != last_gain) {
                REXLOG_DEBUG("USB mics: the game set mic slot {}'s gain to {:.3f} ({:+.1f} dB)",
                             slot + 1, gain, 20.0 * std::log10(ratio));
                NoteUsbMicGain(slot, ratio);
                last_gain = gain;
            }
            Feed(ctx, base, dev, slot, ratio);
            break;
        }
        case Action::kDisconnect:
            Disconnect(ctx, base, dev);
            REX_STORE_U8(mic + kExternalMic_Connected, 0);
            state.Disconnected();
            last_gain = -1.0f;
            NoteUsbMicDisconnect(slot);
            REXLOG_INFO("USB mics: mic slot {} disconnected", slot + 1);
            break;
        case Action::kWait:
            break;
        }
        std::this_thread::sleep_for(UsbMicMonitored(slot) ? kMonitorPollInterval : kPollInterval);
    }

    if (state.connected()) {
        Disconnect(ctx, base, dev);
        REX_STORE_U8(mic + kExternalMic_Connected, 0);
        NoteUsbMicDisconnect(slot);
    }
    ctx.r3.u64 = 0;
}

// MicXbox::AddData(this, data, length), which ExternalMicClientMgr::AddAudio
// calls for the slot's MicXbox: notes which MicXbox that is
extern "C" REX_FUNC(MicXbox__AddData)
{
    if (t_feeding_slot >= 0) {
        g_slot_mic[t_feeding_slot].store(ctx.r3.u32, std::memory_order_relaxed);
    }
    __imp__MicXbox__AddData(ctx, base);
}

namespace {

// FxSend::UpdateMix(send), a virtual
void UpdateMix(const PPCContext& outer, uint8_t* base, uint32_t send) {
    PPCContext ctx = CallContext(outer);
    ctx.r3.u64 = send;
    REX_CALL_INDIRECT_FUNC(REX_LOAD_U32(REX_LOAD_U32(send) + kFxSend_UpdateMix));
}

// the last send of `mic`'s effects, or 0 if it has none
uint32_t LastSend(uint8_t* base, uint32_t mic) {
    uint32_t send = REX_LOAD_U32(mic + kMicXbox_FxSend);
    for (int i = 0; send && i < 16; i++) {
        const uint32_t next = REX_LOAD_U32(send + kFxSend_Next);
        if (!next) return send;
        send = next;
    }
    return 0;
}

void SetDryGain(const PPCContext& ctx, uint8_t* base, uint32_t send, float db) {
    REX_STORE_U32(send + kFxSend_DryGain, std::bit_cast<uint32_t>(db));
    UpdateMix(ctx, base, send);
}

// gives the game back its dry signal once band3 plays no mic
void RestoreDrySignal(const PPCContext& ctx, uint8_t* base) {
    if (!g_dry_send || AnyUsbMicMonitored()) return;
    SetDryGain(ctx, base, g_dry_send, g_dry_send_gain);
    g_dry_send = 0;
}

}

// MicXbox::Poll(this), once a game frame, which sets the playback voice's
// volume from the mic's volume and mute. While band3 plays the slot itself, it
// plays the mic's effects at the volume the game would, and the game plays
// only what comes after: its effects chain's dry signal is turned off, leaving
// the reverb and the overdrive echo. A mic without effects is muted for the
// call instead, so the game's playback stays silent.
extern "C" REX_FUNC(MicXbox__Poll)
{
    const uint32_t mic = ctx.r3.u32;
    const int slot = SlotForMic(mic);
    if (slot < 0 || !UsbMicMonitored(slot)) {
        RestoreDrySignal(ctx, base);
        __imp__MicXbox__Poll(ctx, base);
        return;
    }
    const uint8_t muted = REX_LOAD_U8(mic + kMicXbox_Mute);
    const float volume = std::bit_cast<float>(REX_LOAD_U32(mic + kMicXbox_Volume));
    const bool playing = REX_LOAD_U32(mic + kMicXbox_PlaybackVoice) != 0;
    if (const uint32_t last = LastSend(base, mic)) {
        // every mic shares the one chain (GameMicManager::HookUpFxForMicId)
        if (g_dry_send != last) {
            if (g_dry_send) SetDryGain(ctx, base, g_dry_send, g_dry_send_gain);
            g_dry_send = last;
            g_dry_send_gain = std::bit_cast<float>(REX_LOAD_U32(last + kFxSend_DryGain));
            SetDryGain(ctx, base, last, kSilentDb);
            REXLOG_INFO("USB mics: the game plays only the mics' reverb and echo");
        }
        __imp__MicXbox__Poll(ctx, base);
    } else {
        // the game sets and reads the mute on this thread too
        REX_STORE_U8(mic + kMicXbox_Mute, 1);
        __imp__MicXbox__Poll(ctx, base);
        REX_STORE_U8(mic + kMicXbox_Mute, muted);
    }
    SetUsbMicMonitorGain(slot, MonitorGain(playing, muted != 0, volume));
}
