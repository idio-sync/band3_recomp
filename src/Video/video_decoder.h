#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "src/Video/picture_convert.h"

// Reads a video file's pictures, for the music video player's thread (and
// only that one: Windows' Media Foundation, the one decoder so far, is set
// up per thread). The sound is left alone.

namespace band3::video {

class VideoDecoder {
public:
    virtual ~VideoDecoder() = default;
    // the next picture; false at the video's end or on an error
    virtual bool Read(RgbFrame& out) = 0;
    // reads go on from the key frame at or before `seconds`
    virtual bool Seek(double seconds) = 0;
};

// Sets up and takes down decoding on the calling thread, around any
// OpenVideo there. False if this platform can't decode video.
bool StartVideoThread();
void EndVideoThread();

// the decoder for `path`, or null with what went wrong in `error`
std::unique_ptr<VideoDecoder> OpenVideo(const std::filesystem::path& path, std::string& error);

}
