// No video decoder off Windows yet: music videos are left out there.

#ifndef _WIN32

#include "src/Video/video_decoder.h"

namespace band3::video {

bool StartVideoThread() { return false; }
void EndVideoThread() {}

std::unique_ptr<VideoDecoder> OpenVideo(const std::filesystem::path&, std::string& error) {
    error = "music videos are only decoded on Windows so far";
    return nullptr;
}

}

#endif
