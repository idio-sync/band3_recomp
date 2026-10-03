#include "mic_level.h"
#include <algorithm>
#include <cmath>

namespace band3::launcher {

namespace {

double Seconds(MicLevel::Clock::duration d) { return std::chrono::duration<double>(d).count(); }

// what's left of a level after `elapsed` of falling with `time_constant`
double Fall(double elapsed, std::chrono::milliseconds time_constant) {
    return elapsed <= 0 ? 1.0 : std::exp(-elapsed / Seconds(time_constant));
}

}

void MicLevel::Feed(std::span<const float> samples, int sample_rate, Clock::time_point now) {
    if (samples.empty() || sample_rate <= 0) return;
    // the levels as they stand when these samples start, falling on from there
    const Clock::time_point start =
        now - std::chrono::duration_cast<Clock::duration>(std::chrono::duration<double>(
                  static_cast<double>(samples.size()) / sample_rate));
    const Reading before = Read(start);
    peak_ = before.peak;
    peak_at_ = std::max(peak_at_, start - std::chrono::duration_cast<Clock::duration>(kPeakHold));
    mean_square_ = static_cast<double>(before.rms) * before.rms;

    // a one-pole average of the squares, per sample
    const double keep = std::exp(-1.0 / (Seconds(kRmsWindow) * sample_rate));
    float loudest = 0;
    for (float s : samples) {
        const double v = std::clamp(s, -1.0f, 1.0f);
        mean_square_ = keep * mean_square_ + (1 - keep) * v * v;
        loudest = std::max(loudest, static_cast<float>(std::abs(v)));
    }
    if (loudest >= peak_) {
        peak_ = loudest;
        peak_at_ = now;
    }
    fed_at_ = now;
}

MicLevel::Reading MicLevel::Read(Clock::time_point now) const {
    Reading r;
    const double held = Seconds(now - peak_at_) - Seconds(kPeakHold);
    r.peak = static_cast<float>(peak_ * Fall(held, kPeakFall));
    // with no audio since, the average keeps taking in silence
    r.rms = static_cast<float>(std::sqrt(mean_square_ * Fall(Seconds(now - fed_at_), kRmsWindow)));
    return r;
}

float MicLevel::ToDecibels(float amplitude, float floor) {
    if (amplitude <= 0) return floor;
    return std::max(floor, 20.0f * std::log10(std::min(amplitude, 1.0f)));
}

}
