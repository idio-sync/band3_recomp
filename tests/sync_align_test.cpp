// Checks src/Video/sync_align.h on made-up soundtracks: a "song" of random
// drum-like hits and notes, and a "video" of the same song shifted, at
// another sample rate, quieter, noisier and with its highs cut, as a music
// video's mix is; the offset found must be the shift, and an unrelated
// soundtrack mustn't pass as a match.

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <vector>
#include "src/Video/sync_align.h"

using namespace band3::video;

namespace {

struct Rng {
    uint32_t s;
    float Next() {  // 0..1
        s = s * 1664525u + 1013904223u;
        return float(s >> 8) / float(1u << 24);
    }
};

// `seconds` of a busy, irregular rhythm at `rate` Hz: kick-like thumps,
// snare-like noise bursts and short notes at random times
std::vector<float> MakeSong(double seconds, int rate, uint32_t seed) {
    Rng rng{seed};
    std::vector<float> out(size_t(seconds * rate), 0.0f);
    double t = 0.0;
    while (t < seconds - 1.0) {
        t += 0.08 + 0.35 * rng.Next();
        const size_t at = size_t(t * rate);
        const int kind = int(rng.Next() * 3);
        const double freq = kind == 0 ? 60.0 : 200.0 + 1800.0 * rng.Next();
        const size_t len = size_t(rate * (kind == 1 ? 0.08 : 0.25));
        for (size_t i = 0; i < len && at + i < out.size(); i++) {
            const double env = std::exp(-double(i) / (rate * 0.04));
            const double tone = std::sin(2.0 * std::numbers::pi * freq * double(i) / rate);
            const double noise = rng.Next() * 2.0 - 1.0;
            out[at + i] += float(0.5 * env * (kind == 1 ? noise : tone));
        }
    }
    return out;
}

// `song` (at song_rate) as a video soundtrack at video_rate: the song from
// `from` seconds placed at `at` seconds in the video, quieter, with noise
// and a one-pole low-pass
std::vector<float> MakeVideo(const std::vector<float>& song, int song_rate, int video_rate,
                             double from, double at, double seconds) {
    Rng rng{99};
    std::vector<float> out(size_t(seconds * video_rate), 0.0f);
    float lp = 0.0f;
    for (size_t i = 0; i < out.size(); i++) {
        const double t = double(i) / video_rate - at + from;
        float s = 0.0f;
        if (t >= 0.0) {
            const double pos = t * song_rate;
            const size_t k = size_t(pos);
            if (k + 1 < song.size()) {
                const double f = pos - double(k);
                s = float(song[k] * (1.0 - f) + song[k + 1] * f);
            }
        }
        lp += 0.35f * (s - lp);
        out[i] = 0.4f * lp + 0.02f * (rng.Next() * 2.0f - 1.0f);
    }
    return out;
}

std::vector<float> Envelope(const std::vector<float>& samples, int rate) {
    EnvelopeBuilder b(rate);
    // as a stream feeds it, a block at a time
    for (size_t i = 0; i < samples.size(); i += 2048)
        b.Add(std::span<const float>(samples).subspan(i, std::min<size_t>(2048, samples.size() - i)));
    return b.Frames();
}

}

TEST_CASE("an envelope has kEnvelopeRate frames a second, at any sample rate") {
    for (int rate : {44100, 48000, 22050}) {
        CAPTURE(rate);
        const std::vector<float> e = Envelope(std::vector<float>(size_t(rate) * 3, 0.0f), rate);
        CHECK(std::abs(int(e.size()) - 3 * kEnvelopeRate) <= 3);
    }
}

TEST_CASE("a video whose song starts later is found that much later") {
    const std::vector<float> song = MakeSong(90.0, 44100, 7);
    // the song's start 7.25 s into the video (an intro before it)
    const std::vector<float> video = MakeVideo(song, 44100, 48000, 0.0, 7.25, 100.0);
    // the game hands over the song's first 40 s
    const std::vector<float> song40(song.begin(), song.begin() + 44100 * 40);
    const SyncResult r = AlignEnvelopes(Envelope(song40, 44100), Envelope(video, 48000));
    REQUIRE(r.found);
    CHECK(std::abs(r.offset - 7.25) < 0.01);
    CHECK(r.confident);
}

TEST_CASE("a video that starts into the song gets a negative offset") {
    const std::vector<float> song = MakeSong(90.0, 44100, 11);
    // the video begins 5 s into the song
    const std::vector<float> video = MakeVideo(song, 44100, 44100, 5.0, 0.0, 80.0);
    const std::vector<float> song40(song.begin(), song.begin() + 44100 * 40);
    const SyncResult r = AlignEnvelopes(Envelope(song40, 44100), Envelope(video, 44100));
    REQUIRE(r.found);
    CHECK(std::abs(r.offset - -5.0) < 0.01);
    CHECK(r.confident);
}

TEST_CASE("an unrelated soundtrack isn't taken for a match") {
    const std::vector<float> song = MakeSong(40.0, 44100, 3);
    const std::vector<float> other = MakeSong(90.0, 48000, 1234);
    const SyncResult r = AlignEnvelopes(Envelope(song, 44100), Envelope(other, 48000));
    CHECK_FALSE(r.confident);
}

TEST_CASE("silence finds nothing to be sure of") {
    const std::vector<float> quiet(44100 * 30, 0.0f);
    const SyncResult r =
        AlignEnvelopes(Envelope(quiet, 44100), Envelope(MakeSong(60.0, 48000, 5), 48000));
    CHECK_FALSE(r.confident);
}
