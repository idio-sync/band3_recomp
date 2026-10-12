#include "src/Video/still_check.h"

#include <algorithm>
#include <cstdlib>

#include "src/Video/video_decoder.h"

namespace band3::video {

namespace {

// grey levels (of 255) the pictures change by, on average, below which the
// video counts as still: a moving video changes by tens, a still one by
// little more than its compression's noise
constexpr double kStillChange = 3.0;

}

Thumb MakeThumb(const RgbFrame& frame) {
    Thumb out(size_t(kThumbWidth) * kThumbHeight, 0);
    if (frame.width == 0 || frame.height == 0 || frame.pixels.size() < size_t(frame.width) * frame.height)
        return out;
    for (int y = 0; y < kThumbHeight; y++) {
        const uint32_t y0 = uint32_t(uint64_t(y) * frame.height / kThumbHeight);
        const uint32_t y1 = std::max(y0 + 1, uint32_t(uint64_t(y + 1) * frame.height / kThumbHeight));
        for (int x = 0; x < kThumbWidth; x++) {
            const uint32_t x0 = uint32_t(uint64_t(x) * frame.width / kThumbWidth);
            const uint32_t x1 = std::max(x0 + 1, uint32_t(uint64_t(x + 1) * frame.width / kThumbWidth));
            uint64_t sum = 0, n = 0;
            // every other pixel each way is plenty
            for (uint32_t py = y0; py < y1; py += 2) {
                for (uint32_t px = x0; px < x1; px += 2) {
                    const uint32_t p = frame.pixels[size_t(py) * frame.width + px];
                    sum += ((p >> 16) & 0xff) * 299 + ((p >> 8) & 0xff) * 587 + (p & 0xff) * 114;
                    n++;
                }
            }
            out[size_t(y) * kThumbWidth + x] = uint8_t(n ? sum / (n * 1000) : 0);
        }
    }
    return out;
}

bool LooksStill(const std::vector<Thumb>& thumbs) {
    if (thumbs.size() < 2) return false;
    double total = 0;
    size_t pairs = 0;
    for (size_t i = 1; i < thumbs.size(); i++) {
        const Thumb& a = thumbs[i - 1];
        const Thumb& b = thumbs[i];
        if (a.size() != b.size() || a.empty()) continue;
        uint64_t change = 0;
        for (size_t k = 0; k < a.size(); k++) change += uint64_t(std::abs(int(a[k]) - int(b[k])));
        total += double(change) / double(a.size());
        pairs++;
    }
    return pairs > 0 && total / double(pairs) < kStillChange;
}

std::optional<bool> IsStillVideo(const std::filesystem::path& video, double duration,
                                 std::string& error, int samples) {
    std::unique_ptr<VideoDecoder> decoder = OpenVideo(video, error);
    if (!decoder) return std::nullopt;
    std::vector<Thumb> thumbs;
    RgbFrame frame;
    if (duration <= 0) duration = samples * 3.0;
    // a file shorter than it was said to be: the spots over half as long,
    // until most of them can be read
    for (; duration > 0.25 && int(thumbs.size()) < (samples + 1) / 2; duration /= 2) {
        thumbs.clear();
        for (int i = 0; i < samples; i++) {
            // spread through it, past its first and short of its last seconds
            const double t = duration * (i + 0.5) / samples;
            if (!decoder->Seek(t)) break;
            // the picture at the spot, not the key frame before it, nor the
            // last before the end when the spot's past it
            bool got = false;
            while (decoder->Read(frame)) {
                if (frame.time >= t - 0.05) {
                    got = true;
                    break;
                }
            }
            if (!got) break;
            thumbs.push_back(MakeThumb(frame));
        }
    }
    if (thumbs.size() < 2) {
        error = "its pictures couldn't be read";
        return std::nullopt;
    }
    return LooksStill(thumbs);
}

}
