// Media Foundation's source reader, the decoder when FFmpeg's isn't there:
// decodes whatever Windows has codecs for (H.264 out of the box; HEVC, VP9 and
// AV1 with their Store extensions) and converts to RGB32 itself, so each
// video's own colour matrix (BT.709 for HD) is honoured before
// picture_convert.h turns it into the movie planes' BT.601.

#ifdef _WIN32

#include "src/Video/video_decoder.h"
#include "src/Video/video_decoders.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <format>

using Microsoft::WRL::ComPtr;

namespace band3::video {

namespace {

std::string Hr(const char* what, HRESULT hr) {
    return std::format("{} failed (0x{:08X})", what, uint32_t(hr));
}

class MfDecoder : public VideoDecoder {
public:
    explicit MfDecoder(ComPtr<IMFSourceReader> reader) : reader_(std::move(reader)) {}

    // the output type's size, stride, aperture and pixel shape
    bool Configure(std::string& error) {
        ComPtr<IMFMediaType> type;
        HRESULT hr = reader_->GetCurrentMediaType(
            DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), &type);
        if (FAILED(hr)) {
            error = Hr("GetCurrentMediaType", hr);
            return false;
        }
        UINT32 w = 0, h = 0;
        MFGetAttributeSize(type.Get(), MF_MT_FRAME_SIZE, &w, &h);
        width_ = w;
        height_ = h;
        stride_ = LONG(MFGetAttributeUINT32(type.Get(), MF_MT_DEFAULT_STRIDE, w * 4));
        // decoders pad to their blocks (1920x1088); the aperture is the picture
        MFVideoArea area{};
        crop_x_ = crop_y_ = 0;
        crop_w_ = w;
        crop_h_ = h;
        if (SUCCEEDED(type->GetBlob(MF_MT_MINIMUM_DISPLAY_APERTURE,
                                    reinterpret_cast<UINT8*>(&area), sizeof(area), nullptr))) {
            crop_x_ = uint32_t(std::max(0, int(area.OffsetX.value)));
            crop_y_ = uint32_t(std::max(0, int(area.OffsetY.value)));
            crop_w_ = std::min(uint32_t(area.Area.cx), w - std::min(w, crop_x_));
            crop_h_ = std::min(uint32_t(area.Area.cy), h - std::min(h, crop_y_));
        }
        UINT32 num = 1, den = 1;
        MFGetAttributeRatio(type.Get(), MF_MT_PIXEL_ASPECT_RATIO, &num, &den);
        pixel_aspect_ = num && den ? float(num) / float(den) : 1.0f;
        if (!crop_w_ || !crop_h_) {
            error = std::format("no picture size ({}x{})", w, h);
            return false;
        }
        return true;
    }

    bool Read(RgbFrame& out) override {
        for (;;) {
            DWORD flags = 0;
            LONGLONG time = 0;
            ComPtr<IMFSample> sample;
            const HRESULT hr = reader_->ReadSample(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), 0,
                                                   nullptr, &flags, &time, &sample);
            if (FAILED(hr) || (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)))
                return false;
            if (flags & MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED) {
                std::string error;
                if (!Configure(error)) return false;
            }
            // a gap in the stream
            if (!sample) continue;
            if (!Copy(sample.Get(), out)) return false;
            out.time = double(time) / 1e7;
            return true;
        }
    }

    bool Seek(double seconds) override {
        PROPVARIANT position;
        PropVariantInit(&position);
        position.vt = VT_I8;
        position.hVal.QuadPart = LONGLONG(std::llround(std::max(0.0, seconds) * 1e7));
        const HRESULT hr = reader_->SetCurrentPosition(GUID_NULL, position);
        return SUCCEEDED(hr);
    }

private:
    bool Copy(IMFSample* sample, RgbFrame& out) {
        ComPtr<IMFMediaBuffer> buffer;
        if (FAILED(sample->ConvertToContiguousBuffer(&buffer))) return false;
        // a 2D buffer knows its pitch (negative bottom up); a plain one has
        // the type's default stride
        ComPtr<IMF2DBuffer> buffer2d;
        BYTE* scan0 = nullptr;
        LONG pitch = 0;
        BYTE* data = nullptr;
        DWORD length = 0;
        if (SUCCEEDED(buffer.As(&buffer2d)) && SUCCEEDED(buffer2d->Lock2D(&scan0, &pitch))) {
        } else {
            buffer2d.Reset();
            if (FAILED(buffer->Lock(&data, nullptr, &length))) return false;
            pitch = stride_;
            scan0 = pitch < 0 ? data + size_t(height_ - 1) * size_t(-pitch) : data;
            if (size_t(std::abs(pitch)) * height_ > length) {
                buffer->Unlock();
                return false;
            }
        }
        out.width = crop_w_;
        out.height = crop_h_;
        out.pixel_aspect = pixel_aspect_;
        out.pixels.resize(size_t(crop_w_) * crop_h_);
        for (uint32_t y = 0; y < crop_h_; y++) {
            const BYTE* row = scan0 + ptrdiff_t(y + crop_y_) * pitch + ptrdiff_t(crop_x_) * 4;
            std::memcpy(out.pixels.data() + size_t(y) * crop_w_, row, size_t(crop_w_) * 4);
        }
        if (buffer2d)
            buffer2d->Unlock2D();
        else
            buffer->Unlock();
        return true;
    }

    ComPtr<IMFSourceReader> reader_;
    uint32_t width_ = 0, height_ = 0;
    LONG stride_ = 0;
    uint32_t crop_x_ = 0, crop_y_ = 0, crop_w_ = 0, crop_h_ = 0;
    float pixel_aspect_ = 1.0f;
};

}

bool StartMfThread() {
    if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) return false;
    if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) {
        CoUninitialize();
        return false;
    }
    return true;
}

void EndMfThread() {
    MFShutdown();
    CoUninitialize();
}

std::unique_ptr<VideoDecoder> OpenMfVideo(const std::filesystem::path& path,
                                          std::string& error) {
    ComPtr<IMFAttributes> attributes;
    HRESULT hr = MFCreateAttributes(&attributes, 1);
    if (FAILED(hr)) {
        error = Hr("MFCreateAttributes", hr);
        return nullptr;
    }
    // the reader's own colour conversion, to RGB32
    attributes->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);
    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(path.c_str(), attributes.Get(), &reader);
    if (FAILED(hr)) {
        error = Hr("opening it", hr);
        return nullptr;
    }
    reader->SetStreamSelection(DWORD(MF_SOURCE_READER_ALL_STREAMS), FALSE);
    hr = reader->SetStreamSelection(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), TRUE);
    if (FAILED(hr)) {
        error = Hr("finding its video", hr);
        return nullptr;
    }
    ComPtr<IMFMediaType> type;
    MFCreateMediaType(&type);
    type->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    type->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    hr = reader->SetCurrentMediaType(DWORD(MF_SOURCE_READER_FIRST_VIDEO_STREAM), nullptr,
                                     type.Get());
    if (FAILED(hr)) {
        error = hr == MF_E_TOPO_CODEC_NOT_FOUND || hr == MF_E_INVALIDMEDIATYPE
                    ? Hr("finding a decoder for its video (is its codec's extension installed?)",
                         hr)
                    : Hr("converting its video to RGB", hr);
        return nullptr;
    }
    auto decoder = std::make_unique<MfDecoder>(std::move(reader));
    if (!decoder->Configure(error)) return nullptr;
    return decoder;
}

}

#endif
