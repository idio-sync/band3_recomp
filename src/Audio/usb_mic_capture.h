#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>
#include "usb_mic.h"

// Records the microphones the game's mic slots sing through (see usb_mic.h),
// with SDL, as 16 kHz mono 16-bit big-endian PCM. Turned on by the usb_mics
// setting; usb_mic_devices picks the microphones, one per slot, and
// usb_mic_test_tone replaces them with a steady tone in the first slot.
// Microphones are picked up when plugged in and let go when unplugged.

namespace band3::audio {

// Reads the settings and starts recording when usb_mics is on. Call once at
// startup, before the game creates its mic threads.
void StartUsbMics();
void StopUsbMics();

// whether StartUsbMics started recording; while it didn't, the game's own mic
// threads run
bool UsbMicsRunning();

// whether a microphone (or the test tone) feeds mic slot `slot` (0-3)
bool UsbMicReady(int slot);

// the PCM ready for `slot`, up to out.size() bytes; returns the bytes written
size_t ReadUsbMic(int slot, std::span<uint8_t> out);

// The game side (Hooks/usb_mic.cpp) reports its progress here, for the
// Instrument Lab.
void NoteUsbMicThread(int slot);
void NoteUsbMicConnect(int slot, bool accepted);
void NoteUsbMicDisconnect(int slot);
void NoteUsbMicFed(int slot, size_t bytes);

struct UsbMicSlotStatus {
    // what records the slot; empty while nothing does
    std::string device;
    // the game started this slot's thread and band3's loop runs it
    bool thread = false;
    // the game took the connection (ExternalMicClientProxy::OnMicConnected)
    bool connected = false;
    // connections the game turned down, before it had set the slot up
    uint64_t refusals = 0;
    // audio handed to the game (ExternalMicClientMgr::AddAudio)
    uint64_t bytes_fed = 0;
};

struct UsbMicStatus {
    bool running = false;
    // the test tone's pitch, 0 when microphones are recorded
    int test_tone = 0;
    // every recording device SDL lists
    std::vector<std::string> devices;
    std::array<UsbMicSlotStatus, usb_mic::kSlots> slots{};
};

UsbMicStatus GetUsbMicStatus();

}
