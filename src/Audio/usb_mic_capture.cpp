#include "usb_mic_capture.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <rex/logging.h>
#include "src/settings.h"
#include "usb_mic.h"

namespace band3::audio {

namespace {

using namespace usb_mic;

// how often to look for microphones, and notice them going away
constexpr std::chrono::milliseconds kScanInterval{2000};

// what the game takes: MicXbox::GetSampleRate, mono, the console's byte order
constexpr SDL_AudioSpec kGameSpec{SDL_AUDIO_S16BE, 1, kSampleRate};

struct Device {
    SDL_AudioDeviceID id;
    std::string name;
};

struct MicSlot {
    // part of a device name to record, from usb_mic_devices
    std::string wanted;
    // with usb_mic_devices empty, slot 1 records the default device, which SDL
    // keeps following when the default changes
    bool use_default = false;
    SDL_AudioStream* stream = nullptr;
    SDL_AudioDeviceID device = 0;
    std::string name;
    // logged that it isn't there or won't open, so each scan doesn't repeat it
    bool reported_missing = false;
};

class Capture {
public:
    bool Start() {
        const int tone = REXCVAR_GET(usb_mic_test_tone);
        if (tone > 0) {
            std::lock_guard<std::mutex> lock(mutex_);
            tone_.emplace(static_cast<double>(tone), Clock::now());
            tone_hz_ = tone;
            running_ = true;
            REXLOG_INFO("USB mics: singing a {} Hz test tone into mic slot 1", tone);
            return true;
        }

        if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
            REXLOG_WARN("USB mics: audio recording isn't available ({})", SDL_GetError());
            return false;
        }
        sdl_audio_ = true;

        auto names = ParseDeviceList(REXCVAR_GET(usb_mic_devices));
        if (names.size() > static_cast<size_t>(kSlots)) {
            REXLOG_WARN("USB mics: the game has {} mic slots; ignoring microphones after the {}th",
                        kSlots, kSlots);
            names.resize(kSlots);
        }
        if (names.empty()) {
            slots_[0].use_default = true;
        } else {
            for (size_t i = 0; i < names.size(); i++) slots_[i].wanted = names[i];
        }

        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = true;
        }
        watcher_ = std::thread([this] { WatchLoop(); });
        return true;
    }

    void Stop() {
        {
            std::lock_guard<std::mutex> lock(stop_mutex_);
            stop_ = true;
        }
        stop_cv_.notify_all();
        if (watcher_.joinable()) watcher_.join();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            running_ = false;
            tone_.reset();
            for (auto& slot : slots_) Close(slot);
        }
        if (sdl_audio_) SDL_QuitSubSystem(SDL_INIT_AUDIO);
        sdl_audio_ = false;
    }

    bool Running() {
        std::lock_guard<std::mutex> lock(mutex_);
        return running_;
    }

    bool Ready(int slot) {
        if (slot < 0 || slot >= kSlots) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return false;
        if (tone_) return slot == 0;
        return slots_[slot].stream != nullptr;
    }

    // the game side's progress, reported by Hooks/usb_mic.cpp
    template <typename F>
    void UpdateGameSide(int slot, F update) {
        if (slot < 0 || slot >= kSlots) return;
        std::lock_guard<std::mutex> lock(mutex_);
        update(game_side_[slot]);
    }

    UsbMicStatus Status() {
        std::lock_guard<std::mutex> lock(mutex_);
        UsbMicStatus status;
        status.running = running_;
        status.test_tone = tone_ ? tone_hz_ : 0;
        status.devices = device_names_;
        for (int i = 0; i < kSlots; i++) {
            status.slots[i] = game_side_[i];
            if (tone_) {
                if (i == 0) status.slots[i].device = "test tone";
            } else if (slots_[i].stream) {
                status.slots[i].device = slots_[i].name;
            }
        }
        return status;
    }

    size_t Read(int slot, std::span<uint8_t> out) {
        if (slot < 0 || slot >= kSlots) return 0;
        // whole samples only
        out = out.first(out.size() & ~size_t{1});
        std::lock_guard<std::mutex> lock(mutex_);
        if (!running_) return 0;
        if (tone_) return slot == 0 ? tone_->Read(out, Clock::now()) : 0;

        SDL_AudioStream* stream = slots_[slot].stream;
        if (!stream) return 0;
        DropBacklog(stream);
        const int got = SDL_GetAudioStreamData(stream, out.data(), static_cast<int>(out.size()));
        return got > 0 ? static_cast<size_t>(got) : 0;
    }

private:
    // audio the game hasn't taken for a while (a loading screen, a stall) is
    // dropped rather than played late
    static void DropBacklog(SDL_AudioStream* stream) {
        std::array<uint8_t, kMaxChunk> discard;
        int available = SDL_GetAudioStreamAvailable(stream);
        if (available <= static_cast<int>(kMaxBacklogBytes)) return;
        while (available > static_cast<int>(kMaxChunk)) {
            const int want = std::min(available - static_cast<int>(kMaxChunk),
                                      static_cast<int>(discard.size()));
            if (SDL_GetAudioStreamData(stream, discard.data(), want) <= 0) return;
            available = SDL_GetAudioStreamAvailable(stream);
        }
    }

    static void Close(MicSlot& slot) {
        if (slot.stream) SDL_DestroyAudioStream(slot.stream);
        slot.stream = nullptr;
        slot.device = 0;
    }

    void WatchLoop() {
        std::unique_lock<std::mutex> lock(stop_mutex_);
        while (!stop_) {
            lock.unlock();
            Scan();
            lock.lock();
            stop_cv_.wait_for(lock, kScanInterval, [this] { return stop_.load(); });
        }
    }

    static std::vector<Device> ListDevices() {
        std::vector<Device> devices;
        int count = 0;
        SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count);
        if (!ids) return devices;
        for (int i = 0; i < count; i++) {
            const char* name = SDL_GetAudioDeviceName(ids[i]);
            devices.push_back({ids[i], name ? name : ""});
        }
        SDL_free(ids);
        return devices;
    }

    // opens each slot's microphone when it appears, closes it when it goes
    void Scan() {
        const std::vector<Device> devices = ListDevices();
        std::lock_guard<std::mutex> lock(mutex_);
        device_names_.clear();
        for (const auto& d : devices) device_names_.push_back(d.name);
        for (int i = 0; i < kSlots; i++) {
            MicSlot& slot = slots_[i];
            if (slot.stream) {
                // SDL moves a default-device stream to the new default itself,
                // so it only goes when no microphone is left
                const bool present =
                    slot.use_default ? !devices.empty()
                                     : std::ranges::any_of(devices, [&](const Device& d) {
                                           return d.id == slot.device;
                                       });
                if (present) continue;
                REXLOG_INFO("USB mics: {} (mic slot {}) disconnected", slot.name, i + 1);
                Close(slot);
                continue;
            }
            if (!slot.use_default && slot.wanted.empty()) continue;

            std::optional<Device> device;
            if (slot.use_default) {
                if (devices.empty()) {
                    ReportMissing(slot, i, "a microphone");
                    continue;
                }
                device = Device{SDL_AUDIO_DEVICE_DEFAULT_RECORDING, "the default microphone"};
            } else {
                for (const auto& d : devices) {
                    if (NameMatches(d.name, slot.wanted) && !InUse(d.id)) {
                        device = d;
                        break;
                    }
                }
                if (!device) {
                    ReportMissing(slot, i, "\"" + slot.wanted + "\"");
                    continue;
                }
            }

            slot.stream = SDL_OpenAudioDeviceStream(device->id, &kGameSpec, nullptr, nullptr);
            if (!slot.stream) {
                if (!slot.reported_missing) {
                    REXLOG_WARN("USB mics: couldn't record {} ({})", device->name, SDL_GetError());
                }
                slot.reported_missing = true;
                continue;
            }
            SDL_ResumeAudioStreamDevice(slot.stream);
            slot.device = device->id;
            slot.name = device->name;
            slot.reported_missing = false;
            REXLOG_INFO("USB mics: mic slot {} is {}", i + 1, slot.name);
        }
    }

    void ReportMissing(MicSlot& slot, int index, const std::string& what) {
        if (slot.reported_missing) return;
        slot.reported_missing = true;
        REXLOG_INFO("USB mics: waiting for {} for mic slot {}", what, index + 1);
    }

    bool InUse(SDL_AudioDeviceID id) const {
        return std::ranges::any_of(slots_, [id](const MicSlot& s) { return s.stream && s.device == id; });
    }

    std::mutex stop_mutex_;
    std::condition_variable stop_cv_;
    std::atomic<bool> stop_{false};
    std::thread watcher_;
    bool sdl_audio_ = false;

    // shared by the watcher and the game's mic threads
    std::mutex mutex_;
    bool running_ = false;
    std::array<MicSlot, kSlots> slots_{};
    std::optional<ToneSource> tone_;
    int tone_hz_ = 0;
    std::vector<std::string> device_names_;
    // device stays empty here; Status fills it in
    std::array<UsbMicSlotStatus, kSlots> game_side_{};
};

// lives for the whole run: the game's mic threads may still read it while
// band3 shuts down, so stopping only closes the microphones
Capture& GetCapture() {
    static Capture* capture = new Capture();
    return *capture;
}

}

void StartUsbMics() {
    if (!REXCVAR_GET(usb_mics) || GetCapture().Running()) return;
    GetCapture().Start();
}

void StopUsbMics() {
    if (GetCapture().Running()) GetCapture().Stop();
}

bool UsbMicsRunning() { return GetCapture().Running(); }

bool UsbMicReady(int slot) { return GetCapture().Ready(slot); }

size_t ReadUsbMic(int slot, std::span<uint8_t> out) { return GetCapture().Read(slot, out); }

void NoteUsbMicThread(int slot) {
    GetCapture().UpdateGameSide(slot, [](UsbMicSlotStatus& s) { s.thread = true; });
}

void NoteUsbMicConnect(int slot, bool accepted) {
    GetCapture().UpdateGameSide(slot, [accepted](UsbMicSlotStatus& s) {
        s.connected = accepted;
        if (!accepted) s.refusals++;
    });
}

void NoteUsbMicDisconnect(int slot) {
    GetCapture().UpdateGameSide(slot, [](UsbMicSlotStatus& s) { s.connected = false; });
}

void NoteUsbMicFed(int slot, size_t bytes) {
    GetCapture().UpdateGameSide(slot, [bytes](UsbMicSlotStatus& s) { s.bytes_fed += bytes; });
}

void NoteUsbMicGain(int slot, float ratio) {
    GetCapture().UpdateGameSide(slot, [ratio](UsbMicSlotStatus& s) { s.gain = ratio; });
}

UsbMicStatus GetUsbMicStatus() { return GetCapture().Status(); }

}
