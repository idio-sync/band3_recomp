// FFmpeg's libraries: the music videos' decoder wherever band3 is built with
// their headers (cmake/ffmpeg.cmake) and finds them at run time
// (ffmpeg_api.h). They read what YouTube serves and what people rip from it,
// VP9 and AV1 in WebM as well as H.264 in MP4, and the rest of what FFmpeg
// knows. Pictures are converted to the RGB32 rows picture_convert.h takes
// (0xAARRGGBB) by swscale, with each video's own colour matrix and range, as
// Media Foundation's decoder does.

#ifdef BAND3_HAVE_FFMPEG

#include "src/Video/ffmpeg_api.h"
#include "src/Video/sync_align.h"
#include "src/Video/video_decoders.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <bit>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>

namespace band3::video {

namespace {

const FfmpegApi* g_av = nullptr;

// a library's file name for the major version the headers have: avcodec-62.dll
// on Windows (beside band3), libavcodec.so.62 elsewhere
std::string LibraryName(const char* lib, int major) {
#ifdef _WIN32
    return std::string(lib) + "-" + std::to_string(major) + ".dll";
#else
    return "lib" + std::string(lib) + ".so." + std::to_string(major);
#endif
}

void* OpenLibrary(const std::string& name) {
#ifdef _WIN32
    // beside band3 (its own dependencies, swresample, from there too)
    return LoadLibraryExA(name.c_str(), nullptr,
                          LOAD_LIBRARY_SEARCH_APPLICATION_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
#else
    return dlopen(name.c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
}

void* Symbol(void* library, const char* name) {
#ifdef _WIN32
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(library), name));
#else
    return dlsym(library, name);
#endif
}

std::string AvError(const char* what, int code) {
    char text[AV_ERROR_MAX_STRING_SIZE] = {};
    g_av->av_strerror(code, text, sizeof(text));
    return std::string(what) + ": " + text;
}

// a stream's colour matrix for swscale; unmarked, HD's BT.709 from 720 lines
// up and SD's BT.601 below, as players guess
int SwsMatrix(AVColorSpace space, int height) {
    switch (space) {
        case AVCOL_SPC_BT709: return SWS_CS_ITU709;
        case AVCOL_SPC_BT470BG:
        case AVCOL_SPC_SMPTE170M: return SWS_CS_ITU601;
        case AVCOL_SPC_SMPTE240M: return SWS_CS_SMPTE240M;
        case AVCOL_SPC_FCC: return SWS_CS_FCC;
        case AVCOL_SPC_BT2020_NCL:
        case AVCOL_SPC_BT2020_CL: return SWS_CS_BT2020;
        default: return height >= 720 ? SWS_CS_ITU709 : SWS_CS_ITU601;
    }
}

class FfmpegDecoder : public VideoDecoder {
public:
    ~FfmpegDecoder() override {
        g_av->sws_freeContext(sws_);
        g_av->av_frame_free(&frame_);
        g_av->av_packet_free(&packet_);
        g_av->avcodec_free_context(&codec_);
        g_av->avformat_close_input(&format_);
    }

    bool Open(const std::filesystem::path& path, std::string& error) {
        // FFmpeg takes UTF-8 paths, on Windows too
        const std::u8string u8 = path.u8string();
        const std::string name(u8.begin(), u8.end());
        int ret = g_av->avformat_open_input(&format_, name.c_str(), nullptr, nullptr);
        if (ret < 0) {
            error = AvError("opening it", ret);
            return false;
        }
        if ((ret = g_av->avformat_find_stream_info(format_, nullptr)) < 0) {
            error = AvError("reading its streams", ret);
            return false;
        }
        const AVCodec* codec = nullptr;
        stream_ = g_av->av_find_best_stream(format_, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        if (stream_ < 0 || !codec) {
            error = stream_ == AVERROR_DECODER_NOT_FOUND ? "no decoder for its video"
                                                         : "no video in it";
            return false;
        }
        // the sound and the rest aren't read
        for (unsigned i = 0; i < format_->nb_streams; i++)
            if (int(i) != stream_) format_->streams[i]->discard = AVDISCARD_ALL;
        const AVStream* stream = format_->streams[stream_];
        codec_ = g_av->avcodec_alloc_context3(codec);
        if (!codec_ || g_av->avcodec_parameters_to_context(codec_, stream->codecpar) < 0) {
            error = "setting up its decoder failed";
            return false;
        }
        // a few threads, not the machine's every core: the game runs beside it
        codec_->thread_count = int(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
        if ((ret = g_av->avcodec_open2(codec_, codec, nullptr)) < 0) {
            error = AvError("opening its decoder", ret);
            return false;
        }
        time_base_ = av_q2d(stream->time_base);
        start_ = stream->start_time != AV_NOPTS_VALUE ? stream->start_time : 0;
        packet_ = g_av->av_packet_alloc();
        frame_ = g_av->av_frame_alloc();
        if (!packet_ || !frame_) {
            error = "out of memory";
            return false;
        }
        return true;
    }

    bool Read(RgbFrame& out) override {
        for (;;) {
            int ret = g_av->avcodec_receive_frame(codec_, frame_);
            if (ret == 0) {
                const bool ok = Convert(out);
                g_av->av_frame_unref(frame_);
                if (ok) return true;
                continue;
            }
            if (ret != AVERROR(EAGAIN)) return false;  // the end, or broken
            if (draining_) return false;
            ret = g_av->av_read_frame(format_, packet_);
            if (ret < 0) {
                // the end of the file: what the decoder still holds comes out
                draining_ = true;
                g_av->avcodec_send_packet(codec_, nullptr);
                continue;
            }
            if (packet_->stream_index == stream_) g_av->avcodec_send_packet(codec_, packet_);
            g_av->av_packet_unref(packet_);
        }
    }

    bool Seek(double seconds) override {
        const int64_t target = start_ + int64_t(std::max(0.0, seconds) / time_base_);
        if (g_av->av_seek_frame(format_, stream_, target, AVSEEK_FLAG_BACKWARD) < 0) return false;
        g_av->avcodec_flush_buffers(codec_);
        draining_ = false;
        return true;
    }

private:
    bool Convert(RgbFrame& out) {
        const int w = frame_->width, h = frame_->height;
        if (w <= 0 || h <= 0) return false;
        const auto format = AVPixelFormat(frame_->format);
        sws_ = g_av->sws_getCachedContext(sws_, w, h, format, w, h, AV_PIX_FMT_BGRA, SWS_POINT,
                                          nullptr, nullptr, nullptr);
        if (!sws_) return false;
        const int* matrix = g_av->sws_getCoefficients(SwsMatrix(frame_->colorspace, h));
        const bool full_range = frame_->color_range == AVCOL_RANGE_JPEG;
        g_av->sws_setColorspaceDetails(sws_, matrix, full_range,
                                       g_av->sws_getCoefficients(SWS_CS_DEFAULT), 1, 0, 1 << 16,
                                       1 << 16);
        out.width = uint32_t(w);
        out.height = uint32_t(h);
        out.pixels.resize(size_t(w) * h);
        uint8_t* dst[4] = {reinterpret_cast<uint8_t*>(out.pixels.data()), nullptr, nullptr,
                           nullptr};
        const int dst_stride[4] = {w * 4, 0, 0, 0};
        if (g_av->sws_scale(sws_, frame_->data, frame_->linesize, 0, h, dst, dst_stride) != h)
            return false;
        const AVRational sar = frame_->sample_aspect_ratio.num
                                   ? frame_->sample_aspect_ratio
                                   : format_->streams[stream_]->sample_aspect_ratio;
        out.pixel_aspect = sar.num > 0 && sar.den > 0 ? float(av_q2d(sar)) : 1.0f;
        int64_t ts = frame_->best_effort_timestamp;
        if (ts == AV_NOPTS_VALUE) ts = frame_->pts;
        out.time = ts == AV_NOPTS_VALUE ? 0.0 : double(ts - start_) * time_base_;
        return true;
    }

    AVFormatContext* format_ = nullptr;
    AVCodecContext* codec_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    SwsContext* sws_ = nullptr;
    int stream_ = -1;
    double time_base_ = 0.0;
    int64_t start_ = 0;
    // the file's read to its end; the decoder's last frames are coming out
    bool draining_ = false;
};

}

const FfmpegApi* LoadFfmpeg(std::string& error) {
    static std::once_flag once;
    static FfmpegApi api;
    static std::string failed;
    std::call_once(once, [] {
        // dependencies first: avutil under avcodec under avformat
        struct Lib {
            const char* name;
            int major;
            void* handle;
        } libs[] = {{"avutil", LIBAVUTIL_VERSION_MAJOR, nullptr},
                    {"avcodec", LIBAVCODEC_VERSION_MAJOR, nullptr},
                    {"avformat", LIBAVFORMAT_VERSION_MAJOR, nullptr},
                    {"swscale", LIBSWSCALE_VERSION_MAJOR, nullptr}};
        for (Lib& lib : libs) {
            const std::string file = LibraryName(lib.name, lib.major);
            lib.handle = OpenLibrary(file);
            if (!lib.handle) {
                failed = "no " + file;
                return;
            }
        }
        auto handle = [&libs](const char* name) {
            for (const Lib& lib : libs)
                if (std::string_view(lib.name) == name) return lib.handle;
            return static_cast<void*>(nullptr);
        };
#define BAND3_FFMPEG_LOAD(lib, name)                                                   \
        api.name = reinterpret_cast<decltype(api.name)>(Symbol(handle(#lib), #name)); \
        if (!api.name) {                                                               \
            failed = std::string(#name) + " missing from " #lib;                      \
            return;                                                                    \
        }
        BAND3_FFMPEG_FUNCTIONS(BAND3_FFMPEG_LOAD)
#undef BAND3_FFMPEG_LOAD
        api.av_log_set_level(AV_LOG_QUIET);
        g_av = &api;
    });
    if (!g_av) error = failed;
    return g_av;
}

bool FfmpegAvailable(std::string& error) { return LoadFfmpeg(error) != nullptr; }

std::unique_ptr<VideoDecoder> OpenFfmpegVideo(const std::filesystem::path& path,
                                              std::string& error) {
    if (!LoadFfmpeg(error)) return nullptr;
    auto decoder = std::make_unique<FfmpegDecoder>();
    if (!decoder->Open(path, error)) return nullptr;
    return decoder;
}

namespace {

// one channel's sample `i` of a decoded audio frame, -1..1
float Sample(const AVFrame* f, int channel, int i, int channels) {
    const auto format = AVSampleFormat(f->format);
    const bool planar = format == AV_SAMPLE_FMT_FLTP || format == AV_SAMPLE_FMT_S16P ||
                        format == AV_SAMPLE_FMT_S32P || format == AV_SAMPLE_FMT_DBLP ||
                        format == AV_SAMPLE_FMT_U8P;
    const uint8_t* data = f->extended_data[planar ? channel : 0];
    const int at = planar ? i : i * channels + channel;
    switch (format) {
        case AV_SAMPLE_FMT_FLT:
        case AV_SAMPLE_FMT_FLTP: return reinterpret_cast<const float*>(data)[at];
        case AV_SAMPLE_FMT_S16:
        case AV_SAMPLE_FMT_S16P: return reinterpret_cast<const int16_t*>(data)[at] / 32768.0f;
        case AV_SAMPLE_FMT_S32:
        case AV_SAMPLE_FMT_S32P:
            return float(reinterpret_cast<const int32_t*>(data)[at] / 2147483648.0);
        case AV_SAMPLE_FMT_DBL:
        case AV_SAMPLE_FMT_DBLP: return float(reinterpret_cast<const double*>(data)[at]);
        case AV_SAMPLE_FMT_U8:
        case AV_SAMPLE_FMT_U8P: return (data[at] - 128) / 128.0f;
        default: return 0.0f;
    }
}

}

std::vector<float> FfmpegSoundtrackEnvelope(const std::filesystem::path& path, double seconds,
                                            std::string& error) {
    if (!LoadFfmpeg(error)) return {};
    const std::u8string u8 = path.u8string();
    const std::string name(u8.begin(), u8.end());
    AVFormatContext* format = nullptr;
    int ret = g_av->avformat_open_input(&format, name.c_str(), nullptr, nullptr);
    if (ret < 0) {
        error = AvError("opening it", ret);
        return {};
    }
    struct Closer {
        AVFormatContext** format;
        AVCodecContext* codec = nullptr;
        AVPacket* packet = nullptr;
        AVFrame* frame = nullptr;
        ~Closer() {
            g_av->av_frame_free(&frame);
            g_av->av_packet_free(&packet);
            g_av->avcodec_free_context(&codec);
            g_av->avformat_close_input(format);
        }
    } close{&format};
    if ((ret = g_av->avformat_find_stream_info(format, nullptr)) < 0) {
        error = AvError("reading its streams", ret);
        return {};
    }
    const AVCodec* codec = nullptr;
    const int stream = g_av->av_find_best_stream(format, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0);
    if (stream < 0 || !codec) {
        error = stream == AVERROR_DECODER_NOT_FOUND ? "no decoder for its sound" : "no sound in it";
        return {};
    }
    for (unsigned i = 0; i < format->nb_streams; i++)
        if (int(i) != stream) format->streams[i]->discard = AVDISCARD_ALL;
    close.codec = g_av->avcodec_alloc_context3(codec);
    if (!close.codec ||
        g_av->avcodec_parameters_to_context(close.codec, format->streams[stream]->codecpar) < 0 ||
        (ret = g_av->avcodec_open2(close.codec, codec, nullptr)) < 0) {
        error = "opening its sound's decoder failed";
        return {};
    }
    close.packet = g_av->av_packet_alloc();
    close.frame = g_av->av_frame_alloc();
    if (!close.packet || !close.frame) {
        error = "out of memory";
        return {};
    }
    std::optional<EnvelopeBuilder> envelope;
    int rate = 0;
    int64_t samples = 0, wanted = 0;
    std::vector<float> mono;
    bool draining = false;
    for (;;) {
        ret = g_av->avcodec_receive_frame(close.codec, close.frame);
        if (ret == 0) {
            const AVFrame* f = close.frame;
            if (!envelope) {
                rate = f->sample_rate;
                if (rate <= 0) break;
                envelope.emplace(rate);
                wanted = int64_t(seconds * rate);
            }
            const int channels = std::max(1, f->ch_layout.nb_channels);
            mono.assign(size_t(f->nb_samples), 0.0f);
            for (int i = 0; i < f->nb_samples; i++) {
                float sum = 0.0f;
                for (int c = 0; c < channels; c++) sum += Sample(f, c, i, channels);
                mono[size_t(i)] = sum / float(channels);
            }
            envelope->Add(mono);
            samples += f->nb_samples;
            g_av->av_frame_unref(close.frame);
            if (samples >= wanted) break;
            continue;
        }
        if (ret != AVERROR(EAGAIN) || draining) break;
        ret = g_av->av_read_frame(format, close.packet);
        if (ret < 0) {
            draining = true;
            g_av->avcodec_send_packet(close.codec, nullptr);
            continue;
        }
        if (close.packet->stream_index == stream) g_av->avcodec_send_packet(close.codec, close.packet);
        g_av->av_packet_unref(close.packet);
    }
    if (!envelope || envelope->Frames().empty()) {
        error = "its sound couldn't be decoded";
        return {};
    }
    return envelope->Frames();
}

}

#endif
