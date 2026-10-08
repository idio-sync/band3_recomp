// Checks the music video decoders (src/Video/video_decoders.h) on
// tests/data's clips, each a 64x36 picture (padded to 64x48 in the stream), a
// second of red then a second of blue at 30 fps, key frames every half
// second: H.264 in MP4, VP9 in WebM, AV1 in MKV. FFmpeg's decoder reads all
// three, where band3_tests is built with it (cmake/ffmpeg.cmake); Media
// Foundation's (Windows) the H.264 one, skipped where Windows has no H.264
// decoder (an N edition, a server without its media features).

#include <doctest/doctest.h>
#include <memory>
#include <string>
#include "src/Video/sync_align.h"
#include "src/Video/video_decoders.h"
#include <cstdlib>
#include <vector>

using namespace band3::video;

namespace {

std::string Clip(const char* name) { return std::string(BAND3_ROOT_DIR) + "/tests/data/" + name; }

// the middle pixel's R G B
void Middle(const RgbFrame& f, int rgb[3]) {
    const uint32_t p = f.pixels[size_t(f.height / 2) * f.width + f.width / 2];
    rgb[0] = int((p >> 16) & 0xff);
    rgb[1] = int((p >> 8) & 0xff);
    rgb[2] = int(p & 0xff);
}

bool Red(const RgbFrame& f) {
    int c[3];
    Middle(f, c);
    return c[0] > 200 && c[1] < 60 && c[2] < 60;
}

bool Blue(const RgbFrame& f) {
    int c[3];
    Middle(f, c);
    return c[2] > 200 && c[0] < 60 && c[1] < 60;
}

// what every decoder must do with a red-blue clip
void CheckClip(VideoDecoder& video) {
    RgbFrame f;
    REQUIRE(video.Read(f));
    // the picture, not the stream's padding
    CHECK(f.width == 64);
    CHECK(f.height == 36);
    CHECK(f.pixels.size() == 64 * 36);
    CHECK(f.time == doctest::Approx(0.0).epsilon(0.001));
    CHECK(Red(f));

    int frames = 1;
    double last = f.time;
    bool ordered = true;
    while (video.Read(f)) {
        ordered = ordered && f.time > last;
        last = f.time;
        frames++;
    }
    CHECK(ordered);
    CHECK(frames == 60);
    CHECK(last == doctest::Approx(59.0 / 30.0).epsilon(0.01));
    CHECK(Blue(f));

    // from the key frame at or before: 1.5 s is one
    REQUIRE(video.Seek(1.6));
    REQUIRE(video.Read(f));
    CHECK(f.time <= 1.6);
    CHECK(f.time >= 1.0);
    CHECK(Blue(f));
    REQUIRE(video.Seek(0.2));
    REQUIRE(video.Read(f));
    CHECK(f.time <= 0.2);
    CHECK(Red(f));
}

}

#ifdef BAND3_HAVE_FFMPEG
TEST_CASE("FFmpeg reads H.264, VP9 and AV1 clips' pictures and times, and seeks") {
    std::string error;
    REQUIRE_MESSAGE(FfmpegAvailable(error), error);
    for (const char* name : {"music_video_red_blue.mp4", "music_video_red_blue.webm",
                             "music_video_red_blue_av1.mkv"}) {
        CAPTURE(name);
        auto video = OpenFfmpegVideo(Clip(name), error);
        REQUIRE_MESSAGE(video, error);
        CheckClip(*video);
    }
}

TEST_CASE("FFmpeg says why a file isn't a video") {
    std::string error;
    REQUIRE(FfmpegAvailable(error));
    CHECK_FALSE(OpenFfmpegVideo(Clip("missing.webm"), error));
    CHECK_FALSE(error.empty());
}
#endif

#ifdef _WIN32
TEST_CASE("Media Foundation reads an H.264 clip's pictures and times, and seeks") {
    if (!StartMfThread()) {
        MESSAGE("skipped: no Media Foundation");
        return;
    }
    std::string error;
    auto video = OpenMfVideo(Clip("music_video_red_blue.mp4"), error);
    if (!video && error.find("decoder") != std::string::npos) {
        MESSAGE("skipped: " << error);
        EndMfThread();
        return;
    }
    REQUIRE_MESSAGE(video, error);
    CheckClip(*video);
    video.reset();
    EndMfThread();
}
#endif

TEST_CASE("OpenVideo opens a clip with whatever this build decodes with") {
    std::string error;
    if (!StartVideoThread(error)) {
        MESSAGE("skipped: " << error);
        return;
    }
    auto video = OpenVideo(Clip("music_video_red_blue.mp4"), error);
#if !defined(BAND3_HAVE_FFMPEG)
    // Media Foundation alone, which may have no H.264 decoder
    if (!video && error.find("decoder") != std::string::npos) {
        MESSAGE("skipped: " << error);
        EndVideoThread();
        return;
    }
#endif
    REQUIRE_MESSAGE(video, error);
    RgbFrame f;
    CHECK(video->Read(f));
    video.reset();
    EndVideoThread();
}

#ifdef BAND3_HAVE_FFMPEG
TEST_CASE("a soundtrack decoded from two codecs aligns to the delay between them") {
    // the same irregular tones, as Opus in WebM, and as AAC in M4A with 2.5 s
    // of silence before them (tests/data, made with ffmpeg's aevalsrc)
    std::string error;
    const std::vector<float> opus = SoundtrackEnvelope(Clip("sync_clicks.webm"), 60.0, error);
    REQUIRE_MESSAGE(!opus.empty(), error);
    const std::vector<float> aac = SoundtrackEnvelope(Clip("sync_clicks_late.m4a"), 60.0, error);
    REQUIRE_MESSAGE(!aac.empty(), error);
    // about kEnvelopeRate frames a second of each
    CHECK(std::abs(int(opus.size()) - 15 * kEnvelopeRate) < kEnvelopeRate / 2);
    CHECK(std::abs(int(aac.size()) - int(17.5 * kEnvelopeRate)) < kEnvelopeRate / 2);
    // the Opus one's first 12 s, as a song captured from its start
    const std::vector<float> song(opus.begin(), opus.begin() + 12 * kEnvelopeRate);
    const SyncResult r = AlignEnvelopes(song, aac, -5.0, 10.0, 8.0);
    REQUIRE(r.found);
    CAPTURE(r.score);
    CAPTURE(r.margin);
    CAPTURE(r.onset_score);
    // the video's time at the song's start: its 2.5 s of silence
    CHECK(std::abs(r.offset - 2.5) < 0.02);
    CHECK(r.confident);
    CHECK(SoundtrackEnvelope(Clip("music_video_red_blue.mp4"), 10.0, error).empty());
}
#endif
