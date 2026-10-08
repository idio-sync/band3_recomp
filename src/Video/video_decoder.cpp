#include "src/Video/video_decoder.h"

#include "src/Video/video_decoders.h"

namespace band3::video {

namespace {
// what this thread decodes with, from StartVideoThread
thread_local bool t_ffmpeg = false;
thread_local bool t_mf = false;
}

bool StartVideoThread(std::string& error) {
    std::string why;
#ifdef BAND3_HAVE_FFMPEG
    t_ffmpeg = FfmpegAvailable(why);
#else
    why = "band3 was built without FFmpeg";
#endif
#ifdef _WIN32
    t_mf = StartMfThread();
    if (!t_mf && !t_ffmpeg) why += "; Media Foundation didn't start";
#endif
    if (!t_ffmpeg && !t_mf) error = why;
    return t_ffmpeg || t_mf;
}

void EndVideoThread() {
#ifdef _WIN32
    if (t_mf) EndMfThread();
#endif
    t_ffmpeg = t_mf = false;
}

std::vector<float> SoundtrackEnvelope(const std::filesystem::path& path, double seconds,
                                      std::string& error) {
#ifdef BAND3_HAVE_FFMPEG
    return FfmpegSoundtrackEnvelope(path, seconds, error);
#else
    (void)path;
    (void)seconds;
    error = "band3 was built without FFmpeg";
    return {};
#endif
}

std::unique_ptr<VideoDecoder> OpenVideo(const std::filesystem::path& path, std::string& error) {
    std::string ffmpeg_error;
#ifdef BAND3_HAVE_FFMPEG
    if (t_ffmpeg) {
        if (auto decoder = OpenFfmpegVideo(path, ffmpeg_error)) return decoder;
    }
#endif
#ifdef _WIN32
    if (t_mf) {
        std::string mf_error;
        if (auto decoder = OpenMfVideo(path, mf_error)) return decoder;
        error = ffmpeg_error.empty() ? mf_error : "FFmpeg: " + ffmpeg_error + "; Media Foundation: " + mf_error;
        return nullptr;
    }
#endif
    error = ffmpeg_error.empty() ? "nothing here decodes video" : ffmpeg_error;
    return nullptr;
}

}
