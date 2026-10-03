#pragma once
#include <chrono>
#include <span>

// A microphone's level for the launcher's meters (mic_meter.h), worked out
// from its samples: the peak, held for a moment and then falling, and the RMS
// over about the last third of a second, both falling away when no audio comes.
// Time is passed in rather than read, so this is testable.

namespace band3::launcher {

class MicLevel {
public:
    using Clock = std::chrono::steady_clock;

    // how long a peak is held before it falls
    static constexpr std::chrono::milliseconds kPeakHold{500};
    // how quickly the peak falls after that, and the RMS's window: the time
    // each takes to drop to about a third (an exponential's time constant)
    static constexpr std::chrono::milliseconds kPeakFall{300};
    static constexpr std::chrono::milliseconds kRmsWindow{300};

    // amplitudes, 0 (silence) to 1 (full scale)
    struct Reading {
        float peak = 0;
        float rms = 0;
    };

    // `samples` (-1 to 1) at `sample_rate`, the last of them recorded at `now`
    void Feed(std::span<const float> samples, int sample_rate, Clock::time_point now);
    Reading Read(Clock::time_point now) const;

    // an amplitude in dB below full scale, for drawing: 0 at full scale, and
    // `floor` (and nothing quieter) at silence
    static float ToDecibels(float amplitude, float floor = -60.0f);

private:
    float peak_ = 0;
    Clock::time_point peak_at_{};
    double mean_square_ = 0;
    Clock::time_point fed_at_{};
};

}
