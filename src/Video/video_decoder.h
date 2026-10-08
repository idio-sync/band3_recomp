#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "src/Video/picture_convert.h"

// Reads a video file's pictures, for the music video player's thread (and
// only that one: Windows' Media Foundation is set up per thread). FFmpeg
// first (video_decoder_ffmpeg.cpp), where band3 is built with it and, on
// Windows, its DLLs are beside it; Media Foundation (video_decoder_mf.cpp) on
// Windows otherwise. The sound is left alone.

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
// OpenVideo there. False, with why in `error`, if nothing here can decode
// video.
bool StartVideoThread(std::string& error);
void EndVideoThread();

// the decoder for `path`, or null with what went wrong in `error`
std::unique_ptr<VideoDecoder> OpenVideo(const std::filesystem::path& path, std::string& error);

// The video's soundtrack, its first `seconds` mixed to mono, as a rhythm
// envelope (sync_align.h), for auto-sync; empty, with why in `error`, without
// one or without FFmpeg (Media Foundation's reader isn't used for sound)
std::vector<float> SoundtrackEnvelope(const std::filesystem::path& path, double seconds,
                                      std::string& error);

}
