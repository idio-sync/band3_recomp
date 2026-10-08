// Checks the music video decoder (src/Video/video_decoder_mf.cpp) on
// tests/data/music_video_red_blue.mp4: a 64x36 H.264 clip (padded to 64x48 in
// the stream), a second of red then a second of blue at 30 fps, key frames
// every half second. Windows only; skipped where Media Foundation can't
// decode H.264 (a Windows without its media features).

#ifdef _WIN32

#include <doctest/doctest.h>
#include <string>
#include "src/Video/video_decoder.h"

using namespace band3::video;

namespace {

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

}

TEST_CASE("Media Foundation reads a video's pictures and times, and seeks") {
    if (!StartVideoThread()) {
        MESSAGE("skipped: no Media Foundation");
        return;
    }
    std::string error;
    auto video = OpenVideo(std::string(BAND3_ROOT_DIR) + "/tests/data/music_video_red_blue.mp4",
                           error);
    if (!video && error.find("decoder") != std::string::npos) {
        MESSAGE("skipped: " << error);
        EndVideoThread();
        return;
    }
    REQUIRE_MESSAGE(video, error);

    RgbFrame f;
    REQUIRE(video->Read(f));
    // the picture, not the stream's padding
    CHECK(f.width == 64);
    CHECK(f.height == 36);
    CHECK(f.pixels.size() == 64 * 36);
    CHECK(f.time == doctest::Approx(0.0).epsilon(0.001));
    CHECK(Red(f));

    int frames = 1;
    double last = f.time;
    bool ordered = true;
    while (video->Read(f)) {
        ordered = ordered && f.time > last;
        last = f.time;
        frames++;
    }
    CHECK(ordered);
    CHECK(frames == 60);
    CHECK(last == doctest::Approx(59.0 / 30.0).epsilon(0.01));
    CHECK(Blue(f));

    // from the key frame at or before: 1.5 s is one
    REQUIRE(video->Seek(1.6));
    REQUIRE(video->Read(f));
    CHECK(f.time <= 1.6);
    CHECK(f.time >= 1.0);
    CHECK(Blue(f));
    REQUIRE(video->Seek(0.2));
    REQUIRE(video->Read(f));
    CHECK(f.time <= 0.2);
    CHECK(Red(f));

    video.reset();
    EndVideoThread();
}

#endif
