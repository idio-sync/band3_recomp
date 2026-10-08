#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

// Finds a music video's offset against its song by their soundtracks: each
// becomes a rhythm envelope (how sharply the sound rises, kEnvelopeRate
// frames a second, in a few frequency bands so a different mix or EQ matters
// little), and the video's is slid along the song's for the best match. The
// result is the video's time at the song's start, video_start_time
// (video_files.h). Kept apart from the game and the decoder for unit tests.

namespace band3::video {

// frames a second of a rhythm envelope: 5 ms each
inline constexpr int kEnvelopeRate = 200;

// Builds a rhythm envelope from mono samples at `rate` Hz, a block at a time,
// so a stream can be fed as it plays.
class EnvelopeBuilder {
public:
    explicit EnvelopeBuilder(int rate);
    void Add(std::span<const float> samples);
    // the frames so far, one per 1/kEnvelopeRate s
    const std::vector<float>& Frames() const { return frames_; }

private:
    struct Biquad {
        float b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1 = 0, z2 = 0;
        float Run(float x) {
            const float y = b0 * x + z1;
            z1 = b1 * x - a1 * y + z2;
            z2 = b2 * x - a2 * y;
            return y;
        }
    };
    static constexpr int kBands = 4;
    int rate_;
    Biquad bands_[kBands];
    float energy_[kBands] = {};
    float last_log_[kBands] = {};
    // samples added, and in the frame being summed: frames end where
    // samples * kEnvelopeRate / rate does, so no rate drifts (44.1 kHz's
    // 220.5 samples a frame)
    int64_t samples_ = 0;
    int in_frame_ = 0;
    bool first_ = true;
    std::vector<float> frames_;
};

struct SyncResult {
    bool found = false;
    // the video's time at the song's start, seconds (video_start_time / 1000)
    double offset = 0.0;
    // the song's shape's best match (normalized correlation, 0..1) and how
    // far it stands above the next best more than a second away (1 = no
    // better), and the onsets' match where they line up
    double score = 0.0;
    double margin = 0.0;
    double onset_score = 0.0;
    // whether it's good enough to write without asking
    bool confident = false;
};

// The video envelope's offset against the song's: song frame t matches video
// frame t + offset * kEnvelopeRate. Searched from min_offset to max_offset
// seconds; a match needs at least min_overlap seconds of the song inside the
// video.
SyncResult AlignEnvelopes(std::span<const float> song, std::span<const float> video,
                          double min_offset = -60.0, double max_offset = 180.0,
                          double min_overlap = 15.0);

// how confident a match must be to be written without asking
inline constexpr double kMinScore = 0.4;
inline constexpr double kMinMargin = 1.3;
inline constexpr double kMinOnsetScore = 0.08;

}
