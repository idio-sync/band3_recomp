#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include "generated/band3_init.h"
#include "src/Audio/usb_mic.h"
#include "src/Audio/usb_mic_capture.h"
#include "src/Game/Symbol.h"

// Replaces the thread RB3 runs for each mic slot while usb_mics is on (see
// Audio/usb_mic.h). The game's thread reads the XMic library; this one connects
// the slot when a microphone feeds it and hands the game its audio the way
// ExternalMic::dataReady does. Structures and calls are from rb3-xenon's
// src/system/synth_xbox/ExternalMic.cpp and Mic.cpp.

extern "C" void __imp__ExternalMicThreadEntry(PPCContext& ctx, uint8_t* base);
REX_EXTERN(ExternalMicClientMgr__GetMasterForIndex);
REX_EXTERN(ExternalMicClientProxy__OnMicConnected);
REX_EXTERN(ExternalMicClientMgr__AddAudio);
REX_EXTERN(ExternalMicClientMgr__OnMicDisconnected);

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

// ExternalMicClientMgr::AddAudio(dev, buffer, length), for all the audio ready
void Feed(PPCContext& ctx, uint8_t* base, uint32_t dev, int slot) {
    const uint32_t buffer = ctx.r1.u32 - kAudioBuffer;
    std::array<uint8_t, kMaxChunk> pcm;
    while (true) {
        const size_t length = ReadUsbMic(slot, pcm);
        if (length == 0) return;
        std::memcpy(REX_RAW_ADDR(buffer), pcm.data(), length);
        PPCContext call = CallContext(ctx);
        call.r3.u64 = dev;
        call.r4.u64 = buffer;
        call.r5.u64 = length;
        ExternalMicClientMgr__AddAudio(call, base);
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
        case Action::kFeed:
            Feed(ctx, base, dev, slot);
            break;
        case Action::kDisconnect:
            Disconnect(ctx, base, dev);
            REX_STORE_U8(mic + kExternalMic_Connected, 0);
            state.Disconnected();
            NoteUsbMicDisconnect(slot);
            REXLOG_INFO("USB mics: mic slot {} disconnected", slot + 1);
            break;
        case Action::kWait:
            break;
        }
        std::this_thread::sleep_for(kPollInterval);
    }

    if (state.connected()) {
        Disconnect(ctx, base, dev);
        REX_STORE_U8(mic + kExternalMic_Connected, 0);
        NoteUsbMicDisconnect(slot);
    }
    ctx.r3.u64 = 0;
}
