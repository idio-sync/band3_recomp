#pragma once
#include <cstddef>
#include <cstdint>
#include <span>

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

}
