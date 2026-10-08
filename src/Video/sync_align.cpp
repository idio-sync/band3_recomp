#include "src/Video/sync_align.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace band3::video {

namespace {

// RBJ band-pass (constant 0 dB peak) at f0 Hz, bandwidth q
void BandPass(float* b0, float* b1, float* b2, float* a1, float* a2, double f0, double q,
              int rate) {
    const double w = 2.0 * std::numbers::pi * f0 / rate;
    const double alpha = std::sin(w) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    *b0 = float(alpha / a0);
    *b1 = 0.0f;
    *b2 = float(-alpha / a0);
    *a1 = float(-2.0 * std::cos(w) / a0);
    *a2 = float((1.0 - alpha) / a0);
}

// mean 0, deviation 1 (all zeros stay zeros)
std::vector<float> Normalize(std::span<const float> x) {
    std::vector<float> out(x.begin(), x.end());
    if (out.empty()) return out;
    double mean = 0;
    for (float v : out) mean += v;
    mean /= double(out.size());
    double var = 0;
    for (float& v : out) {
        v = float(v - mean);
        var += double(v) * v;
    }
    const double sd = std::sqrt(var / double(out.size()));
    if (sd > 1e-9)
        for (float& v : out) v = float(v / sd);
    return out;
}

// each frame's mean over the `window` frames round it
std::vector<float> Smooth(std::span<const float> x, int window) {
    std::vector<double> sum(x.size() + 1, 0.0);
    for (size_t i = 0; i < x.size(); i++) sum[i + 1] = sum[i] + x[i];
    std::vector<float> out(x.size());
    const long half = window / 2;
    for (long i = 0; i < long(x.size()); i++) {
        const long a = std::max(0L, i - half), b = std::min(long(x.size()), i + half + 1);
        out[size_t(i)] = float((sum[size_t(b)] - sum[size_t(a)]) / double(b - a));
    }
    return out;
}

// every `step`th frame's sum over each run of `step`: an envelope at a
// lower rate
std::vector<float> Decimate(std::span<const float> x, int step) {
    std::vector<float> out(x.size() / size_t(step), 0.0f);
    for (size_t i = 0; i < out.size(); i++)
        for (int k = 0; k < step; k++) out[i] += x[i * step + k];
    return out;
}

// normalized correlation of song against video moved by `lag` frames (video
// frame t + lag against song frame t), over the frames both have; -2 when
// they share fewer than min_frames
double Correlate(const std::vector<float>& song, const std::vector<float>& video, long lag,
                 long min_frames) {
    const long t0 = std::max(0L, -lag);
    const long t1 = std::min(long(song.size()), long(video.size()) - lag);
    if (t1 - t0 < min_frames) return -2.0;
    double sv = 0, ss = 0, vv = 0;
    for (long t = t0; t < t1; t++) {
        const double s = song[size_t(t)], v = video[size_t(t + lag)];
        sv += s * v;
        ss += s * s;
        vv += v * v;
    }
    if (ss <= 0 || vv <= 0) return 0.0;
    return sv / std::sqrt(ss * vv);
}

}

EnvelopeBuilder::EnvelopeBuilder(int rate) : rate_(std::max(rate, 1)) {
    // lows (kick, bass), low mids (snare body), high mids (guitars, voice),
    // highs (cymbals, attacks)
    static constexpr double kCentres[kBands] = {90.0, 400.0, 1600.0, 6000.0};
    for (int b = 0; b < kBands; b++) {
        const double f0 = std::min(kCentres[b], rate_ * 0.45);
        BandPass(&bands_[b].b0, &bands_[b].b1, &bands_[b].b2, &bands_[b].a1, &bands_[b].a2, f0,
                 0.7, rate_);
    }
}

void EnvelopeBuilder::Add(std::span<const float> samples) {
    for (float x : samples) {
        for (int b = 0; b < kBands; b++) {
            const float y = bands_[b].Run(x);
            energy_[b] += y * y;
        }
        samples_++;
        in_frame_++;
        if (samples_ * kEnvelopeRate < int64_t(frames_.size() + 1) * rate_) continue;
        // each band's rise in loudness (log energy), falls left out
        float onset = 0.0f;
        for (int b = 0; b < kBands; b++) {
            const float level = std::log(energy_[b] / float(in_frame_) + 1e-10f);
            if (!first_) onset += std::max(0.0f, level - last_log_[b]);
            last_log_[b] = level;
            energy_[b] = 0.0f;
        }
        first_ = false;
        frames_.push_back(onset);
        in_frame_ = 0;
    }
}

SyncResult AlignEnvelopes(std::span<const float> song_env, std::span<const float> video_env,
                          double min_offset, double max_offset, double min_overlap) {
    SyncResult r;
    if (song_env.empty() || video_env.empty()) return r;
    const std::vector<float> song = Normalize(song_env);
    const std::vector<float> video = Normalize(video_env);

    // 1. the song's shape: onsets summed over half a second, 10 a second.
    // A steady beat matches itself a beat apart, and a verse the next
    // verse, but the loudness of the whole stretch only matches in one
    // place: this picks the place, and says how sure it is.
    constexpr int kShapeStep = kEnvelopeRate / 10;
    constexpr int kShapeWindow = kEnvelopeRate / 2;
    const std::vector<float> song_s = Normalize(Decimate(Smooth(song, kShapeWindow), kShapeStep));
    const std::vector<float> video_s = Normalize(Decimate(Smooth(video, kShapeWindow), kShapeStep));
    const double shape_rate = double(kEnvelopeRate) / kShapeStep;
    const long lag0 = long(std::floor(min_offset * shape_rate));
    const long lag1 = long(std::ceil(max_offset * shape_rate));
    std::vector<double> shape(size_t(lag1 - lag0 + 1), -2.0);
    for (long lag = lag0; lag <= lag1; lag++)
        shape[size_t(lag - lag0)] = Correlate(song_s, video_s, lag, long(min_overlap * shape_rate));
    const auto best_it = std::max_element(shape.begin(), shape.end());
    if (best_it == shape.end() || *best_it <= -1.5) return r;
    const long best_s = lag0 + long(best_it - shape.begin());
    // the next best place more than a second away: another peak, not the
    // best one's slope (a song whose loudness barely changes has a broad one)
    double second = 0.0;
    for (long lag = lag0 + 1; lag < lag1; lag++) {
        const double c = shape[size_t(lag - lag0)];
        if (std::abs(lag - best_s) <= long(shape_rate) || c < shape[size_t(lag - lag0 - 1)] ||
            c < shape[size_t(lag - lag0 + 1)])
            continue;
        second = std::max(second, c);
    }

    // 2. the onsets themselves, within half a second of it: to 5 ms
    const long min_frames = long(min_overlap * kEnvelopeRate);
    const long centre = best_s * kShapeStep;
    long best = centre;
    double best_score = -2.0;
    for (long lag = centre - kEnvelopeRate / 2; lag <= centre + kEnvelopeRate / 2; lag++) {
        const double c = Correlate(song, video, lag, min_frames);
        if (c > best_score) {
            best_score = c;
            best = lag;
        }
    }
    if (best_score <= -1.5) return r;
    // between frames: the parabola through the best and its neighbours
    double frac = 0.0;
    const double left = Correlate(song, video, best - 1, min_frames);
    const double right = Correlate(song, video, best + 1, min_frames);
    const double denom = left - 2.0 * best_score + right;
    if (left > -1.5 && right > -1.5 && std::abs(denom) > 1e-12)
        frac = std::clamp(0.5 * (left - right) / denom, -0.5, 0.5);

    r.found = true;
    r.offset = (double(best) + frac) / kEnvelopeRate;
    r.score = std::max(0.0, *best_it);
    r.margin = second > 1e-9 ? *best_it / second : 99.0;
    r.onset_score = std::max(0.0, best_score);
    r.confident = r.score >= kMinScore && r.margin >= kMinMargin && r.onset_score >= kMinOnsetScore;
    return r;
}

}
