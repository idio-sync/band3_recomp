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

// AlignSegments' windows over the song's shape (20 frames a second): 10 s
// long, 2.5 s apart
constexpr int kShapePerSecond = 20;
// how much each window's shape is smoothed (envelope frames): little, so its
// onsets have to line up, not just its loudness; a repetitive song's 10 s
// windows otherwise match strongly almost anywhere (Buddy Holly's did)
constexpr int kWindowSmoothing = kEnvelopeRate * 15 / 100;
constexpr long kWindow = 10 * kShapePerSecond;
constexpr long kHop = 5 * kShapePerSecond / 2;
// a window has to overlap the video this much to be matched there
constexpr long kMinWindowOverlap = kWindow * 6 / 10;
// each window's best places kept, at least this far apart (frames)
constexpr int kPeaks = 4;
constexpr long kPeakSpacing = kShapePerSecond;
// the offsets a path may take: those whose windows' votes (each window's
// best few places, weighed by their match) add up to this, the best so many
constexpr size_t kVotesPerWindow = 3;
constexpr double kMinVotes = 1.5;
constexpr size_t kMaxOffsets = 24;
// a window matches a place no better than this: as good as no match
constexpr double kNoMatch = 0.35;
// the path's cost of changing offset; an offset within kSameLag frames of the
// last is the same
constexpr double kJumpCost = 2.0;
// and a change's cost per second it moves the offset: a video's own cuts
// (an intro skit, a radio edit) move it seconds to tens of seconds, while a
// riff or chorus heard again elsewhere is often minutes away (in real videos,
// 70 to 125 s), so a big move needs a long stretch that holds it
constexpr double kJumpCostPerSecond = 0.05;
constexpr long kSameLag = 2;
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
    double score = 0.0;        // the mean of the windows that match (not weak)
    int matched = 0;           // how many do
};

}

SegmentMap AlignSegments(std::span<const float> song_env, std::span<const float> video_env,
                         double min_offset, double max_offset) {
    SegmentMap map;
    if (song_env.size() < size_t(kEnvelopeRate) * 15 || video_env.empty()) return map;
    constexpr int kStep = kEnvelopeRate / kShapePerSecond;
    const std::vector<float> song = Normalize(song_env);
    const std::vector<float> video = Normalize(video_env);
    const std::vector<float> song_s = Normalize(Decimate(Smooth(song, kWindowSmoothing), kStep));
    const std::vector<float> video_s = Normalize(Decimate(Smooth(video, kWindowSmoothing), kStep));
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
    // a window's match at a place, give or take the frames that count as it
    const auto score_at = [&](size_t w, long lag) {
        double best = 0;
        for (long l = lag - kSameLag; l <= lag + kSameLag; l++)
            if (l >= lag0 && l <= lag1) best = std::max(best, double(corr[w][size_t(l - lag0)]));
        return best;
    };

    // 2. The offsets a video's parts can be at: those several windows' best
    // places agree on (a video has a few, one per part), each weighed by how
    // well its windows match there. Every window may be at any of them, so
    // the path can hold one through a stretch where it matches only weakly.
    struct Vote {
        long lag;        // the best matched of its windows' places
        double best;     // that one's match
        double weight;   // all of them together
    };
    std::vector<Vote> votes;
    for (size_t w = 0; w < windows; w++) {
        for (size_t i = 0; i < std::min<size_t>(peaks[w].size(), kVotesPerWindow); i++) {
            const long lag = peaks[w][i];
            const double weight = double(corr[w][size_t(lag - lag0)]);
            auto it = std::find_if(votes.begin(), votes.end(),
                                   [&](const Vote& v) { return std::abs(v.lag - lag) <= kSameLag; });
            if (it == votes.end()) {
                votes.push_back({lag, weight, weight});
                continue;
            }
            it->weight += weight;
            if (weight > it->best) {
                it->best = weight;
                it->lag = lag;
            }
        }
    }
    std::sort(votes.begin(), votes.end(), [](const Vote& a, const Vote& b) { return a.weight > b.weight; });
    std::vector<long> offsets;
    for (const Vote& v : votes)
        if (v.weight >= kMinVotes && offsets.size() < kMaxOffsets) offsets.push_back(v.lag);
    if (offsets.empty()) return map;  // nothing matches anywhere
    const std::optional<long> dominant = offsets.front();
    const std::vector<std::vector<long>> states(windows, offsets);
    // Each window's cost at each place: its match there, no worse than no
    // match at all (a weak stretch, a quiet part, is played through at the
    // place the path holds, so the path never leaves a place through one,
    // where a change would cost nothing). Gaps come only after: the weak
    // stretches before the first match and after the last.
    std::vector<std::vector<double>> cost(windows);
    std::vector<std::vector<int>> from(windows);
    const auto emit = [&](size_t w, int k) { return -std::max(score_at(w, states[w][size_t(k)]), kNoMatch); };
    const auto move = [&](size_t pw, int pk, size_t w, int k) {
        const long move_frames = std::abs(states[pw][size_t(pk)] - states[w][size_t(k)]);
        return move_frames <= kSameLag
                   ? 0.0
                   : kJumpCost + kJumpCostPerSecond * double(move_frames) / kShapePerSecond;
    };
    for (size_t w = 0; w < windows; w++) {
        const int count = int(states[w].size());
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

    // 3. runs of windows at one place, the weak ones before the first match
    // and after the last as gaps
    std::vector<std::optional<long>> placed(windows);
    for (size_t w = 0; w < windows; w++) placed[w] = states[w][size_t(path[w])];
    const auto weak = [&](size_t w) { return score_at(w, *placed[w]) < kNoMatch; };
    for (size_t w = 0; w < windows && weak(w); w++) placed[w].reset();
    for (size_t w = windows; w-- > 0 && placed[w] && weak(w);) placed[w].reset();
    std::vector<Run> runs;
    for (size_t w = 0; w < windows; w++) {
        const std::optional<long> lag = placed[w];
        const double score = lag ? score_at(w, *lag) : 0.0;
        if (runs.empty() || runs.back().lag.has_value() != lag.has_value() ||
            (lag && std::abs(*runs.back().lag - *lag) > kSameLag))
            runs.push_back({long(w), long(w), lag, 0.0, 0});
        Run& r = runs.back();
        r.last = long(w);
        // a weak window's played through, not counted as a match
        if (score >= kNoMatch) {
            r.score = (r.score * r.matched + score) / (r.matched + 1);
            r.matched++;
        }
    }
    {
        int in_parts = 0, matched_windows = 0;
        for (const Run& r : runs) {
            if (!r.lag) continue;
            in_parts += int(r.last - r.first + 1);
            matched_windows += r.matched;
        }
        map.support = in_parts ? double(matched_windows) / in_parts : 0.0;
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

    // placing the changes can leave a part too short to be sure of: it goes
    // into the part before (the first, into the one after)
    for (bool merged = true; merged && segments.size() > 1;) {
        merged = false;
        for (size_t i = 0; i < segments.size(); i++) {
            const double end = i + 1 < segments.size() ? segments[i + 1].song_start : total_seconds;
            if (end - segments[i].song_start >= kMinSegmentSeconds) continue;
            if (i == 0) segments[1].song_start = 0.0;
            segments.erase(segments.begin() + long(i));
            merged = true;
            break;
        }
        for (size_t i = 1; i < segments.size(); i++) {
            const VideoSegment& a = segments[i - 1];
            const VideoSegment& b = segments[i];
            if (a.offset.has_value() == b.offset.has_value() &&
                (!a.offset || std::abs(*a.offset - *b.offset) <= kSameOffset)) {
                segments.erase(segments.begin() + long(i));
                merged = true;
                break;
            }
        }
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
                    map.support >= kMinSupport && int(map.segments.size()) - 1 <= kMaxJumps;
    return map;
}

Match MatchSong(std::span<const float> song, std::span<const float> video, double min_overlap) {
    Match m;
    if (song.size() < size_t(kMinMapSeconds * kEnvelopeRate)) {
        m.result = AlignEnvelopes(song, video, -60.0, 180.0, min_overlap);
        return m;
    }
    m.map = AlignSegments(song, video);
    SyncResult& r = m.result;
    r.found = m.map->found;
    r.score = r.onset_score = m.map->score;
    r.coverage = m.map->coverage;
    r.confident = m.map->confident;
    // the parts after a gap before the first: that gap is the video's not
    // having begun
    size_t first = 0;
    while (first < m.map->segments.size() && !m.map->segments[first].offset) first++;
    r.parts = int(m.map->segments.size() - first);
    if (first < m.map->segments.size()) r.offset = *m.map->segments[first].offset;
    // One part throughout that too few windows match surely (a mix far from
    // the song's): sure all the same if the whole song's single offset, found
    // on its own, is sure and the same.
    const size_t matched_parts = size_t(std::count_if(m.map->segments.begin(), m.map->segments.end(),
                                                      [](const VideoSegment& s) { return s.offset.has_value(); }));
    if (!r.confident && m.map->found && matched_parts == 1 && m.map->coverage >= kMinCoverage) {
        const SyncResult whole = AlignEnvelopes(song, video, -120.0, 600.0);
        if (whole.confident && std::abs(whole.offset - r.offset) <= kSameOffset) {
            r.confident = m.map->confident = true;
            r.margin = whole.margin;
        }
    }
    if (r.margin == 0.0) r.margin = r.confident ? 99.0 : 0.0;
    return m;
}

}
