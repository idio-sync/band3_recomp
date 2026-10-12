#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "src/Video/picture_convert.h"

// Whether a downloaded video is a still picture: an "audio" upload, an art
// track or a lyric sheet that never changes, whose sound matches the song
// perfectly, so the match alone can't tell it from the music video. Pictures
// taken at spots spread through it, shrunk to a few grey pixels, are
// compared: if hardly any changes, it's still.

namespace band3::video {

// a picture shrunk to kThumbWidth x kThumbHeight grey levels
inline constexpr int kThumbWidth = 32;
inline constexpr int kThumbHeight = 18;
using Thumb = std::vector<uint8_t>;
Thumb MakeThumb(const RgbFrame& frame);

// whether these, taken through a video, show a still picture: the average
// change between each and the next stays under a few grey levels
bool LooksStill(const std::vector<Thumb>& thumbs);

// LooksStill of `samples` pictures spread over the video's first
// `duration` seconds; none, with why in `error`, if it can't be read. Opens
// the video on this thread: call StartVideoThread (video_decoder.h) first.
std::optional<bool> IsStillVideo(const std::filesystem::path& video, double duration,
                                 std::string& error, int samples = 20);

}
