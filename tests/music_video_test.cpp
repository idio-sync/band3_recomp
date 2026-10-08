// Checks the music video player's pure parts: a picture made into a movie
// frame's planes (src/Video/picture_convert.h), the frame queue's rules for
// what to show and when to read or seek (frame_queue.h), where a song's
// video is found (video_files.h), and when a venue becomes a video venue
// for it (video_venue.h).

#include <doctest/doctest.h>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include "src/Video/frame_queue.h"
#include "src/Video/picture_convert.h"
#include "src/Video/video_files.h"
#include "src/Video/video_venue.h"

using namespace band3::video;
namespace fs = std::filesystem;

namespace {

RgbFrame Solid(uint32_t w, uint32_t h, uint32_t rgb) {
    RgbFrame f;
    f.width = w;
    f.height = h;
    f.pixels.assign(size_t(w) * h, 0xff000000u | rgb);
    return f;
}

// shade.hlsli's MovieRgb, 0..255
void MovieRgb(uint8_t y, uint8_t cr, uint8_t cb, float rgb[3]) {
    const float a = 1.16412353515625f * y / 255.0f;
    const float r = cr / 255.0f, b = cb / 255.0f;
    rgb[0] = 255.0f * (a + 1.595794677734375f * r - 0.8706550598144531f);
    rgb[1] = 255.0f * (a - 0.8134765625f * r - 0.391448974609375f * b + 0.5297050476074219f);
    rgb[2] = 255.0f * (a + 2.017822265625f * b - 1.0816688537597656f);
}

std::shared_ptr<const PlaneSet> Planes() { return std::make_shared<PlaneSet>(); }

}

TEST_CASE("colours made into planes come back through the movie shader") {
    const uint32_t colours[] = {0x000000, 0xffffff, 0xff0000, 0x00ff00, 0x0000ff,
                                0x808080, 0xffa020, 0x3070c0};
    for (uint32_t c : colours) {
        CAPTURE(c);
        PlaneSet p;
        ToPlanes(Solid(32, 18, c), Fit::kStretch, 16.0f / 9.0f, 16, 16, 8, 8, p);
        float rgb[3];
        MovieRgb(p.y.Row(8)[8], p.cr.Row(4)[4], p.cb.Row(4)[4], rgb);
        CHECK(std::abs(rgb[0] - float((c >> 16) & 0xff)) < 3.0f);
        CHECK(std::abs(rgb[1] - float((c >> 8) & 0xff)) < 3.0f);
        CHECK(std::abs(rgb[2] - float(c & 0xff)) < 3.0f);
    }
}

TEST_CASE("fit puts bars where the picture's shape leaves room") {
    // 4:3 on 16:9: bars at the sides
    FitRects r = ComputeFit(4.0f / 3.0f, 16.0f / 9.0f, Fit::kFit);
    CHECK(r.dst[0] == doctest::Approx(0.125f));
    CHECK(r.dst[2] == doctest::Approx(0.875f));
    CHECK(r.dst[1] == 0.0f);
    CHECK(r.dst[3] == 1.0f);
    CHECK(r.src[0] == 0.0f);
    CHECK(r.src[2] == 1.0f);
    // 2.35:1 on 16:9: above and below
    r = ComputeFit(2.35f, 16.0f / 9.0f, Fit::kFit);
    CHECK(r.dst[0] == 0.0f);
    CHECK(r.dst[1] == doctest::Approx((1.0f - 16.0f / 9.0f / 2.35f) / 2));
    // fill cuts the picture instead: a 4:3 one's top and bottom
    r = ComputeFit(4.0f / 3.0f, 16.0f / 9.0f, Fit::kFill);
    CHECK(r.dst[0] == 0.0f);
    CHECK(r.dst[3] == 1.0f);
    CHECK(r.src[1] == doctest::Approx(0.125f));
    CHECK(r.src[3] == doctest::Approx(0.875f));
    // the same shape, or stretch: all of it everywhere
    for (Fit fit : {Fit::kFit, Fit::kFill}) {
        r = ComputeFit(16.0f / 9.0f, 16.0f / 9.0f, fit);
        CHECK(r.dst[0] == 0.0f);
        CHECK(r.src[3] == 1.0f);
    }
    r = ComputeFit(4.0f / 3.0f, 16.0f / 9.0f, Fit::kStretch);
    CHECK(r.dst[0] == 0.0f);
    CHECK(r.src[0] == 0.0f);
    CHECK(ParseFit("fill") == Fit::kFill);
    CHECK(ParseFit("stretch") == Fit::kStretch);
    CHECK(ParseFit("anything") == Fit::kFit);
}

TEST_CASE("a 4:3 picture on stretched 640x720 planes keeps its shape on screen") {
    // video_02's planes: 640x720, which the game stretches over 16:9
    PlaneSet p;
    ToPlanes(Solid(64, 48, 0xffffff), Fit::kFit, 16.0f / 9.0f, 640, 720, 320, 360, p);
    // the screen's middle three quarters across, so the planes' too
    CHECK(p.y.Row(360)[70] == kBlackY);
    CHECK(p.y.Row(360)[90] == 235);
    CHECK(p.y.Row(360)[549] == 235);
    CHECK(p.y.Row(360)[570] == kBlackY);
    CHECK(p.y.Row(0)[320] == 235);
    CHECK(p.y.Row(719)[320] == 235);
    CHECK(p.cr.Row(180)[10] == kNeutralC);
}

TEST_CASE("scaling keeps a picture's halves where they were") {
    // left half red, right half blue, shrunk to a third and enlarged 4x
    for (uint32_t w : {16u, 256u}) {
        CAPTURE(w);
        RgbFrame f = Solid(48, 8, 0xff0000);
        for (uint32_t y = 0; y < 8; y++)
            for (uint32_t x = 24; x < 48; x++) f.pixels[y * 48 + x] = 0xff0000ff;
        PlaneSet p;
        ToPlanes(f, Fit::kStretch, 16.0f / 9.0f, w, 8, w / 2, 4, p);
        CHECK(p.cr.Row(2)[1] > 200);
        CHECK(p.cb.Row(2)[w / 2 - 2] > 200);
        CHECK(p.cr.Row(2)[w / 2 - 2] < 128);
    }
}

TEST_CASE("nearest resize samples texel centres") {
    Plane p;
    p.Resize(4, 4, 0);
    for (uint32_t y = 0; y < 4; y++)
        for (uint32_t x = 0; x < 4; x++) p.Row(y)[x] = uint8_t(y * 4 + x);
    // enlarged, the corners stay the corners
    const Plane big = ResizeNearest(p, 8, 8);
    CHECK(big.width == 8);
    CHECK(big.Row(0)[0] == 0);
    CHECK(big.Row(7)[7] == 15);
    CHECK(big.Row(7)[0] == 12);
    // halved, each output texel takes its 2x2's lower right
    const Plane small = ResizeNearest(p, 2, 2);
    CHECK(small.Row(0)[0] == 5);
    CHECK(small.Row(1)[1] == 15);
}

TEST_CASE("the queue shows the last frame at or before the song's time") {
    FrameQueue q;
    CHECK(q.Show(0.0) == nullptr);
    auto a = Planes(), b = Planes(), c = Planes();
    q.Push({0.0, a});
    q.Push({0.5, b});
    q.Push({1.0, c});
    // before the video: nothing yet
    CHECK(q.Show(-1.0) == nullptr);
    CHECK(q.Size() == 3);
    CHECK(q.Show(0.2) == a);
    CHECK(q.Show(0.49) == b);  // kEarly
    CHECK(q.Size() == 2);
    CHECK(q.AheadOf(0.6) == 1);
    // the last stays past the end
    CHECK(q.Show(30.0) == c);
    CHECK(q.Show(31.0) == c);
    CHECK(q.Size() == 1);
}

TEST_CASE("the decoder reads ahead, waits, and seeks on jumps") {
    // just opened, the song in its count-in: read
    CHECK(NextStep(-3.0, std::nullopt, std::nullopt, 0) == DecodeStep::kRead);
    // enough queued: wait
    CHECK(NextStep(-3.0, 0.1, 0.0, kFramesAhead) == DecodeStep::kWait);
    // a video that starts 30 s in: seek straight there
    CHECK(NextStep(27.0, std::nullopt, std::nullopt, 0) == DecodeStep::kSeek);
    // playing along
    CHECK(NextStep(10.0, 10.05, 9.98, 2) == DecodeStep::kRead);
    // restarted: back before what's queued
    CHECK(NextStep(-2.0, 60.0, 59.9, 3) == DecodeStep::kSeek);
    // practice's next section, far ahead
    CHECK(NextStep(90.0, 60.0, 59.9, 3) == DecodeStep::kSeek);
    // a first frame a little after 0 doesn't send the count-in seeking
    CHECK(NextStep(-3.0, 0.04, 0.04, kFramesAhead) == DecodeStep::kWait);
    CHECK(NextStep(-3.0, 0.04, 0.04, 1) == DecodeStep::kRead);
    // reading up from a key frame 8 s before where it sought: the song moved
    // on 3 s meanwhile, which would seek again but for catching_up
    CHECK(NextStep(63.0, 60.0, 52.0, 0) == DecodeStep::kSeek);
    CHECK(NextStep(63.0, 60.0, 52.0, 0, true) == DecodeStep::kRead);
    CHECK(NextStep(71.0, 60.0, 52.0, 0, true) == DecodeStep::kSeek);
}

TEST_CASE("frames are made into planes at the venue's rate, and now and then while catching up") {
    CHECK(ShouldMake(5.0, 5.0, std::nullopt));
    // 60 fps video: every other frame
    CHECK_FALSE(ShouldMake(5.0167, 5.0, 5.0));
    CHECK(ShouldMake(5.0334, 5.0, 5.0));
    // behind the song after a seek: only every kCatchUpGap
    CHECK_FALSE(ShouldMake(3.1, 5.0, 3.0));
    CHECK(ShouldMake(3.3, 5.0, 3.0));
}

TEST_CASE("video_start_time comes from the .ini, in seconds") {
    CHECK(ParseStartTime("") == 0.0);
    CHECK(ParseStartTime("[song]\r\nname = x\r\nvideo_start_time = 1500\r\n") ==
          doctest::Approx(1.5));
    CHECK(ParseStartTime("video_start_time=-250") == doctest::Approx(-0.25));
    CHECK(ParseStartTime("; video_start_time = 9\nvideo_start_time = 40") ==
          doctest::Approx(0.04));
    CHECK(ParseStartTime("video_start_time = soon") == 0.0);
}

TEST_CASE("a song's video is found by its shortname, first folder first") {
    const fs::path root = fs::temp_directory_path() / "band3_music_video_test";
    fs::remove_all(root);
    fs::create_directories(root / "a");
    fs::create_directories(root / "b");
    std::ofstream(root / "a" / "song1.webm") << "x";
    std::ofstream(root / "b" / "song1.mp4") << "x";
    std::ofstream(root / "b" / "song2.mkv") << "x";
    std::ofstream(root / "b" / "song2.ini") << "[song]\nvideo_start_time = 2000\n";
    const std::vector<fs::path> folders = {root / "a", root / "b", root / "missing"};

    auto v = FindVideo(folders, "song1");
    REQUIRE(v);
    CHECK(v->path == root / "a" / "song1.webm");
    CHECK(v->start_time == 0.0);
    v = FindVideo(folders, "song2");
    REQUIRE(v);
    CHECK(v->path == root / "b" / "song2.mkv");
    CHECK(v->start_time == doctest::Approx(2.0));
    CHECK_FALSE(FindVideo(folders, "song3"));
    CHECK_FALSE(FindVideo(folders, ""));
    CHECK_FALSE(FindVideo(folders, "../b/song1"));
    fs::remove_all(root);
}

TEST_CASE("a song with a video gets a video venue as often as the chance says") {
    VideoVenueInputs in;
    in.music_videos = true;
    in.has_video = true;
    in.chance = 100;
    in.roll = 0.999;
    CHECK(PickVideoVenue(in));
    // 25% of the time: rolls under a quarter
    in.chance = 25;
    in.roll = 0.24;
    CHECK(PickVideoVenue(in));
    in.roll = 0.25;
    CHECK_FALSE(PickVideoVenue(in));
    // off
    in.chance = 0;
    in.roll = 0.0;
    CHECK_FALSE(PickVideoVenue(in));
}

TEST_CASE("a forced venue, the black background, no video or videos off keep the venue") {
    VideoVenueInputs in;
    in.music_videos = true;
    in.has_video = true;
    in.chance = 100;
    for (int i = 0; i < 4; i++) {
        VideoVenueInputs v = in;
        if (i == 0) v.forced_venue = true;
        if (i == 1) v.black_background = true;
        if (i == 2) v.has_video = false;
        if (i == 3) v.music_videos = false;
        CAPTURE(i);
        CHECK_FALSE(PickVideoVenue(v));
    }
}
