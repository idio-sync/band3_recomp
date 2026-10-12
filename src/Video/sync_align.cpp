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

namespace {

// AlignSegments' windows over the song's shape (10 frames a second): 10 s
// long, 2.5 s apart
constexpr int kShapePerSecond = 10;
constexpr long kWindow = 10 * kShapePerSecond;
constexpr long kHop = 5 * kShapePerSecond / 2;
// a window has to overlap the video this much to be matched there
constexpr long kMinWindowOverlap = kWindow * 6 / 10;
// each window's best places kept, at least this far apart (frames)
constexpr int kPeaks = 4;
constexpr long kPeakSpacing = kShapePerSecond;
// a window matches a place no better than this: as good as no match
constexpr double kNoMatch = 0.35;
// the path's costs: changing offset, and going in or out of a gap; an offset
// within kSameLag frames of the last is the same
constexpr double kJumpCost = 2.0;
constexpr double kGapCost = 1.0;
constexpr long kSameLag = 1;
// a segment shorter than this (seconds) goes into a neighbour
constexpr double kMinSegmentSeconds = 8.0;
// how far round its shape's place a segment's offset is looked for to the
// frame, and its change of offset (seconds)
constexpr double kRefineOffset = 0.3;
constexpr double kRefineCut = 6.0;
// segments whose offsets differ by no more than this (seconds) are one
constexpr double kSameOffset = 0.05;

// Pearson correlation of song[t0, t0 + n) with the video `lag` frames on,
// over the frames both have; 0 with fewer than min_n of them, or no variance
double WindowCorr(const std::vector<float>& s, const std::vector<float>& v, long t0, long n, long lag,
                  long min_n) {
    const long a = std::max(t0, -lag), b = std::min(t0 + n, long(v.size()) - lag);
    if (b - a < min_n) return 0.0;
    double ss = 0, sv = 0, s2 = 0, v2 = 0, svp = 0;
    for (long t = a; t < b; t++) {
        const double x = s[size_t(t)], y = v[size_t(t + lag)];
        ss += x;
        sv += y;
        s2 += x * x;
        v2 += y * y;
        svp += x * y;
    }
    const double count = double(b - a);
    const double cov = svp - ss * sv / count;
    const double vs = s2 - ss * ss / count, vv = v2 - sv * sv / count;
    if (vs <= 1e-9 || vv <= 1e-9) return 0.0;
    return cov / std::sqrt(vs * vv);
}

// a run of windows on one place (lag), or none
struct Run {
    long first = 0, last = 0;  // windows
    std::optional<long> lag;   // shape frames
    double score = 0.0;        // the windows' mean
};

}

SegmentMap AlignSegments(std::span<const float> song_env, std::span<const float> video_env,
                         double min_offset, double max_offset) {
    SegmentMap map;
    if (song_env.size() < size_t(kEnvelopeRate) * 15 || video_env.empty()) return map;
    constexpr int kStep = kEnvelopeRate / kShapePerSecond;
    const std::vector<float> song = Normalize(song_env);
    const std::vector<float> video = Normalize(video_env);
    const std::vector<float> song_s = Normalize(Decimate(Smooth(song, kEnvelopeRate / 2), kStep));
    const std::vector<float> video_s = Normalize(Decimate(Smooth(video, kEnvelopeRate / 2), kStep));
    const long length = long(song_s.size());
    const long lag0 = long(std::floor(min_offset * kShapePerSecond));
    const long lag1 = long(std::ceil(max_offset * kShapePerSecond));
    if (length < kWindow || lag1 < lag0) return map;

    // 1. each window's match along the video
    std::vector<long> starts;
    for (long t0 = 0; t0 + kWindow <= length; t0 += kHop) starts.push_back(t0);
    if (starts.back() + kWindow < length) starts.push_back(length - kWindow);
    const size_t windows = starts.size();
    std::vector<std::vector<float>> corr(windows, std::vector<float>(size_t(lag1 - lag0 + 1), 0.0f));
    std::vector<std::vector<long>> peaks(windows);
    for (size_t w = 0; w < windows; w++) {
        std::vector<float>& c = corr[w];
        for (long lag = lag0; lag <= lag1; lag++)
            c[size_t(lag - lag0)] = float(WindowCorr(song_s, video_s, starts[w], kWindow, lag, kMinWindowOverlap));
        // its best places, a second apart at least
        std::vector<long> order;
        for (long i = 1; i + 1 < long(c.size()); i++)
            if (c[size_t(i)] >= kNoMatch * 0.8 && c[size_t(i)] >= c[size_t(i - 1)] && c[size_t(i)] >= c[size_t(i + 1)])
                order.push_back(i);
        std::sort(order.begin(), order.end(), [&](long a, long b) { return c[size_t(a)] > c[size_t(b)]; });
        for (long i : order) {
            if (int(peaks[w].size()) >= kPeaks) break;
            if (std::any_of(peaks[w].begin(), peaks[w].end(),
                            [&](long p) { return std::abs(p - (i + lag0)) < kPeakSpacing; }))
                continue;
            peaks[w].push_back(i + lag0);
        }
    }
    const auto score_at = [&](size_t w, long lag) {
        double best = 0;
        for (long l = lag - 1; l <= lag + 1; l++)
            if (l >= lag0 && l <= lag1) best = std::max(best, double(corr[w][size_t(l - lag0)]));
        return best;
    };

    // 2. a path through them: each window's own places and its neighbours'
    // (so the place the song's been at can go on where a repeat outscores it)
    std::vector<std::vector<long>> states(windows);
    for (size_t w = 0; w < windows; w++) {
        for (size_t n = w == 0 ? 0 : w - 1; n <= std::min(windows - 1, w + 1); n++)
            for (long lag : peaks[n])
                if (std::none_of(states[w].begin(), states[w].end(),
                                 [&](long s) { return std::abs(s - lag) <= kSameLag; }))
                    states[w].push_back(lag);
    }
    // costs per state, the last one being no match
    std::vector<std::vector<double>> cost(windows);
    std::vector<std::vector<int>> from(windows);
    const auto emit = [&](size_t w, int k) {
        return k < int(states[w].size()) ? -score_at(w, states[w][size_t(k)]) : -kNoMatch;
    };
    const auto move = [&](size_t pw, int pk, size_t w, int k) {
        const bool pgap = pk >= int(states[pw].size()), gap = k >= int(states[w].size());
        if (pgap && gap) return 0.0;
        if (pgap || gap) return kGapCost;
        return std::abs(states[pw][size_t(pk)] - states[w][size_t(k)]) <= kSameLag ? 0.0 : kJumpCost;
    };
    for (size_t w = 0; w < windows; w++) {
        const int count = int(states[w].size()) + 1;
        cost[w].assign(size_t(count), 0.0);
        from[w].assign(size_t(count), -1);
        for (int k = 0; k < count; k++) {
            if (w == 0) {
                cost[w][size_t(k)] = emit(w, k);
                continue;
            }
            double best = 1e300;
            for (int pk = 0; pk < int(cost[w - 1].size()); pk++) {
                const double c = cost[w - 1][size_t(pk)] + move(w - 1, pk, w, k);
                if (c < best) {
                    best = c;
                    from[w][size_t(k)] = pk;
                }
            }
            cost[w][size_t(k)] = best + emit(w, k);
        }
    }
    std::vector<int> path(windows);
    path.back() = int(std::min_element(cost.back().begin(), cost.back().end()) - cost.back().begin());
    for (size_t w = windows - 1; w > 0; w--) path[w - 1] = from[w][size_t(path[w])];

    // 3. runs of windows at one place
    std::vector<Run> runs;
    for (size_t w = 0; w < windows; w++) {
        const bool gap = path[w] >= int(states[w].size());
        const std::optional<long> lag = gap ? std::nullopt : std::optional<long>(states[w][size_t(path[w])]);
        const double score = gap ? 0.0 : score_at(w, *lag);
        if (!runs.empty() && runs.back().lag.has_value() == lag.has_value() &&
            (!lag || std::abs(*runs.back().lag - *lag) <= kSameLag)) {
            Run& r = runs.back();
            r.score = (r.score * double(r.last - r.first + 1) + score) / double(r.last - r.first + 2);
            r.last = long(w);
        } else {
            runs.push_back({long(w), long(w), lag, score});
        }
    }
    // where each run starts, in shape frames: halfway between the last
    // window's centre before it and its first's
    const auto centre = [&](long w) { return starts[size_t(w)] + kWindow / 2; };
    const auto run_start = [&](size_t i) {
        return i == 0 ? 0L : (centre(runs[i - 1].last) + centre(runs[i].first)) / 2;
    };
    const auto run_seconds = [&](size_t i) {
        const long end = i + 1 < runs.size() ? run_start(i + 1) : length;
        return double(end - run_start(i)) / kShapePerSecond;
    };
    // a run too short to be sure of goes into the longer of its neighbours
    for (bool merged = true; merged && runs.size() > 1;) {
        merged = false;
        for (size_t i = 0; i < runs.size(); i++) {
            if (run_seconds(i) >= kMinSegmentSeconds) continue;
            const bool before = i > 0 && (i + 1 == runs.size() || run_seconds(i - 1) >= run_seconds(i + 1));
            const size_t into = before ? i - 1 : i + 1;
            runs[into].first = std::min(runs[into].first, runs[i].first);
            runs[into].last = std::max(runs[into].last, runs[i].last);
            runs.erase(runs.begin() + long(i));
            merged = true;
            break;
        }
        // neighbours now at the same place are one
        for (size_t i = 1; i < runs.size(); i++) {
            if (runs[i].lag.has_value() != runs[i - 1].lag.has_value() ||
                (runs[i].lag && std::abs(*runs[i].lag - *runs[i - 1].lag) > kSameLag))
                continue;
            runs[i - 1].last = runs[i].last;
            runs.erase(runs.begin() + long(i));
            merged = true;
            break;
        }
    }

    // 4. each run's offset to the frame, then each change of offset placed
    // where the frames' matches change from one offset to the next
    std::vector<VideoSegment> segments;
    for (size_t i = 0; i < runs.size(); i++) {
        VideoSegment seg;
        seg.song_start = double(run_start(i)) / kShapePerSecond;
        seg.score = runs[i].score;
        if (runs[i].lag) {
            const long t0 = long(seg.song_start * kEnvelopeRate);
            const long t1 = long((seg.song_start + run_seconds(i)) * kEnvelopeRate);
            const long centre_lag = *runs[i].lag * kStep;
            const long reach = long(kRefineOffset * kEnvelopeRate);
            long best = centre_lag;
            double best_c = -2.0;
            for (long lag = centre_lag - reach; lag <= centre_lag + reach; lag++) {
                const double c = WindowCorr(song, video, t0, t1 - t0, lag, (t1 - t0) / 2);
                if (c > best_c) {
                    best_c = c;
                    best = lag;
                }
            }
            const double left = WindowCorr(song, video, t0, t1 - t0, best - 1, (t1 - t0) / 2);
            const double right = WindowCorr(song, video, t0, t1 - t0, best + 1, (t1 - t0) / 2);
            const double denom = left - 2.0 * best_c + right;
            const double frac = std::abs(denom) > 1e-12 ? std::clamp(0.5 * (left - right) / denom, -0.5, 0.5) : 0.0;
            seg.offset = (double(best) + frac) / kEnvelopeRate;
        }
        segments.push_back(seg);
    }
    // A stretch matched nowhere between two that are matched is in the video
    // (a quiet part, or one its mix makes sound otherwise), not missing from
    // it: the video plays on through it, at the offset either side if
    // they're the same, else with the change placed inside it. Only before
    // the first match and after the last is there a gap.
    for (size_t i = 1; i + 1 < segments.size();) {
        if (segments[i].offset || !segments[i - 1].offset || !segments[i + 1].offset) {
            i++;
            continue;
        }
        const double middle = (segments[i].song_start + segments[i + 1].song_start) / 2;
        segments.erase(segments.begin() + long(i));
        segments[i].song_start = middle;
    }
    // neighbours at the same offset (to a frame or so) are one
    const double total_seconds = double(song.size()) / kEnvelopeRate;
    const auto seconds_of = [&](size_t i) {
        return (i + 1 < segments.size() ? segments[i + 1].song_start : total_seconds) - segments[i].song_start;
    };
    for (size_t i = 1; i < segments.size();) {
        VideoSegment& a = segments[i - 1];
        const VideoSegment& b = segments[i];
        const bool same = a.offset.has_value() == b.offset.has_value() &&
                          (!a.offset || std::abs(*a.offset - *b.offset) <= kSameOffset);
        if (!same) {
            i++;
            continue;
        }
        const double wa = seconds_of(i - 1), wb = seconds_of(i);
        if (a.offset) a.offset = (*a.offset * wa + *b.offset * wb) / (wa + wb);
        a.score = (a.score * wa + b.score * wb) / (wa + wb);
        segments.erase(segments.begin() + long(i));
    }

    const auto lag_frames = [](const VideoSegment& s) {
        return s.offset ? std::optional<long>(std::lround(*s.offset * kEnvelopeRate)) : std::nullopt;
    };
    const auto product = [&](long t, std::optional<long> lag) {
        if (!lag || t + *lag < 0 || t + *lag >= long(video.size())) return 0.0;
        return double(song[size_t(t)]) * video[size_t(t + *lag)];
    };
    for (size_t i = 1; i < segments.size(); i++) {
        const long cut = long(segments[i].song_start * kEnvelopeRate);
        const long prev_start = long(segments[i - 1].song_start * kEnvelopeRate);
        const long next_end = i + 1 < segments.size() ? long(segments[i + 1].song_start * kEnvelopeRate)
                                                      : long(song.size());
        const long lo = std::max(prev_start + kEnvelopeRate, cut - long(kRefineCut * kEnvelopeRate));
        const long hi = std::min(next_end - kEnvelopeRate, cut + long(kRefineCut * kEnvelopeRate));
        if (hi <= lo) continue;
        const std::optional<long> a = lag_frames(segments[i - 1]), b = lag_frames(segments[i]);
        // the split s that makes the most of [lo, s) at a and [s, hi) at b
        double run = 0, best = 0;
        long best_s = lo;
        for (long t = lo; t < hi; t++) {
            run += product(t, a) - product(t, b);
            if (run > best) {
                best = run;
                best_s = t + 1;
            }
        }
        segments[i].song_start = double(best_s) / kEnvelopeRate;
    }

    // 5. what it comes to
    const double total = double(song.size()) / kEnvelopeRate;
    double matched = 0, scored = 0;
    for (size_t i = 0; i < segments.size(); i++) {
        if (!segments[i].offset) continue;
        const double end = i + 1 < segments.size() ? segments[i + 1].song_start : total;
        const double seconds = end - segments[i].song_start;
        matched += seconds;
        scored += seconds * segments[i].score;
    }
    map.found = matched > 0;
    map.segments = std::move(segments);
    map.coverage = total > 0 ? matched / total : 0.0;
    map.score = matched > 0 ? scored / matched : 0.0;
    map.confident = map.found && map.coverage >= kMinCoverage && map.score >= kMinSegmentScore &&
                    int(map.segments.size()) - 1 <= kMaxJumps;
    return map;
}

}
