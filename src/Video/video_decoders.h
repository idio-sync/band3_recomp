#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "src/Video/video_decoder.h"

// The decoders OpenVideo (video_decoder.h) picks from, each on its own, for
// video_decoder.cpp and the tests.

namespace band3::video {

#ifdef _WIN32
// Media Foundation: COM and MF started on the calling thread
bool StartMfThread();
void EndMfThread();
std::unique_ptr<VideoDecoder> OpenMfVideo(const std::filesystem::path& path,
                                          std::string& error);
#endif

#ifdef BAND3_HAVE_FFMPEG
// whether FFmpeg's libraries load (ffmpeg_api.h), with why not in `error`
bool FfmpegAvailable(std::string& error);
std::unique_ptr<VideoDecoder> OpenFfmpegVideo(const std::filesystem::path& path,
                                              std::string& error);
std::vector<float> FfmpegSoundtrackEnvelope(const std::filesystem::path& path, double seconds,
                                            std::string& error);
#endif

}
