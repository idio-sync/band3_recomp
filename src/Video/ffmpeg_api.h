#pragma once

#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

// The FFmpeg functions the music video decoder (video_decoder_ffmpeg.cpp)
// calls, looked up at run time from the libraries whose headers band3 was
// built with (avcodec-62.dll beside band3 on Windows, the system's
// libavcodec.so.62 elsewhere: their major versions, so the same ABI), not
// linked: without them band3 still starts, and plays no video through them.

namespace band3::video {

#define BAND3_FFMPEG_FUNCTIONS(X)                                       \
    X(avutil, av_log_set_level)                                         \
    X(avutil, av_strerror)                                              \
    X(avutil, av_frame_alloc)                                           \
    X(avutil, av_frame_free)                                            \
    X(avutil, av_frame_unref)                                           \
    X(avcodec, av_packet_alloc)                                         \
    X(avcodec, av_packet_free)                                          \
    X(avcodec, av_packet_unref)                                         \
    X(avcodec, avcodec_alloc_context3)                                  \
    X(avcodec, avcodec_free_context)                                    \
    X(avcodec, avcodec_parameters_to_context)                           \
    X(avcodec, avcodec_open2)                                           \
    X(avcodec, avcodec_send_packet)                                     \
    X(avcodec, avcodec_receive_frame)                                   \
    X(avcodec, avcodec_flush_buffers)                                   \
    X(avformat, avformat_open_input)                                    \
    X(avformat, avformat_close_input)                                   \
    X(avformat, avformat_find_stream_info)                              \
    X(avformat, av_find_best_stream)                                    \
    X(avformat, av_read_frame)                                          \
    X(avformat, av_seek_frame)                                          \
    X(swscale, sws_getCachedContext)                                    \
    X(swscale, sws_freeContext)                                         \
    X(swscale, sws_getCoefficients)                                     \
    X(swscale, sws_setColorspaceDetails)                                \
    X(swscale, sws_scale)

struct FfmpegApi {
#define BAND3_FFMPEG_POINTER(lib, name) decltype(&::name) name = nullptr;
    BAND3_FFMPEG_FUNCTIONS(BAND3_FFMPEG_POINTER)
#undef BAND3_FFMPEG_POINTER
};

// the functions, loaded once; null, with why in `error`, if a library or a
// function isn't there
const FfmpegApi* LoadFfmpeg(std::string& error);

}
