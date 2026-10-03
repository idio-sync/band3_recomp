#include "mic_meter.h"
#include <SDL3/SDL_audio.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_stdinc.h>
#include <algorithm>
#include <array>
#include <mutex>
#include <vector>
#include <rex/logging.h>
#include "src/Audio/usb_mic.h"

namespace band3::launcher {

namespace {

// mono floats at a rate SDL converts any microphone to cheaply enough
constexpr int kRate = 16000;
constexpr SDL_AudioSpec kSpec{SDL_AUDIO_F32, 1, kRate};

}

struct MicMeter::State {
    mutable std::mutex mutex;
    MicLevel level;
    std::string error;
    std::string name;
    // set while recording; Close takes it, and destroys it outside the lock,
    // since SDL holds the stream's own lock while it calls OnAudio
    SDL_AudioStream* stream = nullptr;
    // SDL's audio subsystem was started for this meter, and not yet stopped
    bool sdl_audio = false;

    // SDL's audio thread, after it puts new audio into the stream
    static void SDLCALL OnAudio(void* user, SDL_AudioStream* stream, int additional, int) {
        auto* self = static_cast<State*>(user);
        std::array<float, 1024> samples;
        int available = std::max(additional, SDL_GetAudioStreamAvailable(stream));
        while (available > 0) {
            const int want = std::min<int>(available, sizeof(samples));
            const int got = SDL_GetAudioStreamData(stream, samples.data(), want);
            if (got <= 0) break;
            available -= got;
            std::lock_guard<std::mutex> lock(self->mutex);
            if (!self->stream) return;
            self->level.Feed({samples.data(), static_cast<size_t>(got) / sizeof(float)}, kRate,
                             MicLevel::Clock::now());
        }
    }

    void Close() {
        SDL_AudioStream* closing;
        bool quit;
        {
            std::lock_guard<std::mutex> lock(mutex);
            closing = stream;
            stream = nullptr;
            quit = sdl_audio;
            sdl_audio = false;
        }
        if (closing) SDL_DestroyAudioStream(closing);
        if (quit) SDL_QuitSubSystem(SDL_INIT_AUDIO);
    }
};

namespace {

std::mutex g_meters_mutex;
// every meter's state, open or not, for CloseMicMeters
std::vector<std::weak_ptr<MicMeter::State>> g_meters;

}

MicMeter::MicMeter(std::string device) : state_(std::make_shared<State>()) {
    {
        std::lock_guard<std::mutex> lock(g_meters_mutex);
        std::erase_if(g_meters, [](const auto& meter) { return meter.expired(); });
        g_meters.push_back(state_);
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        state_->error = std::string("Audio recording isn't available: ") + SDL_GetError();
        return;
    }
    state_->sdl_audio = true;

    SDL_AudioDeviceID id = SDL_AUDIO_DEVICE_DEFAULT_RECORDING;
    std::string name = "the default microphone";
    if (!device.empty()) {
        id = 0;
        int count = 0;
        if (SDL_AudioDeviceID* ids = SDL_GetAudioRecordingDevices(&count)) {
            for (int i = 0; i < count && id == 0; i++) {
                const char* each = SDL_GetAudioDeviceName(ids[i]);
                if (each && audio::usb_mic::NameMatches(each, device)) {
                    id = ids[i];
                    name = each;
                }
            }
            SDL_free(ids);
        }
        if (id == 0) {
            state_->error = "\"" + device + "\" isn't connected";
            return;
        }
    }

    // opened paused; the callback reads the stream from the lock, so it's
    // published before the device starts. Started outside the lock: SDL's
    // audio thread takes the device's lock, then the stream's, then this one.
    SDL_AudioStream* stream = SDL_OpenAudioDeviceStream(id, &kSpec, &State::OnAudio, state_.get());
    {
        std::lock_guard<std::mutex> lock(state_->mutex);
        if (!stream) {
            state_->error = "Couldn't record " + name + ": " + SDL_GetError();
            return;
        }
        state_->stream = stream;
        state_->name = name;
    }
    if (!SDL_ResumeAudioStreamDevice(stream)) {
        REXLOG_WARN("Launcher: couldn't start recording {} ({})", name, SDL_GetError());
    }
}

MicMeter::~MicMeter() { Close(); }

bool MicMeter::recording() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->stream != nullptr;
}

std::string MicMeter::error() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->error;
}

std::string MicMeter::device_name() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->name;
}

MicLevel::Reading MicMeter::Level() const {
    std::lock_guard<std::mutex> lock(state_->mutex);
    return state_->level.Read(MicLevel::Clock::now());
}

void MicMeter::Close() { state_->Close(); }

void CloseMicMeters() {
    std::vector<std::shared_ptr<MicMeter::State>> open;
    {
        std::lock_guard<std::mutex> lock(g_meters_mutex);
        for (const auto& meter : g_meters) {
            if (auto state = meter.lock()) open.push_back(std::move(state));
        }
        g_meters.clear();
    }
    for (const auto& state : open) state->Close();
}

}
