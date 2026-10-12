#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

// A song's audio as Rock Band keeps it, a .mogg: a header (its version, the
// Ogg stream's offset, a seek map) and an Ogg Vorbis stream of all its
// tracks' channels. Version 0x0A is the Ogg as it is, which most customs are;
// later versions are encrypted, and band3 doesn't read them. Kept apart from
// FFmpeg (which decodes the Vorbis, video_decoder_ffmpeg.cpp) for unit tests.

namespace band3::video {

inline constexpr uint32_t kMoggUnencrypted = 0x0A;

struct MoggHeader {
    uint32_t version = 0;
    uint32_t ogg_offset = 0;  // from the file's start
};

// the header; nullopt if it's too short, or its Ogg starts past the end
std::optional<MoggHeader> ParseMoggHeader(std::span<const uint8_t> mogg);

// The packets of an Ogg stream's first logical stream, in order (Vorbis's
// three headers, then its audio), joined across pages. Pages of other streams
// are passed over; anything that isn't a whole page ends it.
class OggPackets {
public:
    explicit OggPackets(std::span<const uint8_t> ogg) : ogg_(ogg) {}
    // the next packet; false at the end
    bool Next(std::vector<uint8_t>& packet);

private:
    bool NextPage();

    std::span<const uint8_t> ogg_;
    size_t at_ = 0;  // the next page's start
    // the page being read: its lacing values and body
    std::span<const uint8_t> lacing_;
    std::span<const uint8_t> body_;
    size_t segment_ = 0;
    size_t body_at_ = 0;
    std::optional<uint32_t> serial_;
    // a packet's start before the first page we read: passed over
    bool skipping_ = false;
};

// Vorbis's three header packets as FFmpeg's decoder takes them (extradata):
// 2, the first two's sizes in Xiph lacing, then the three
std::vector<uint8_t> XiphExtradata(std::span<const uint8_t> id, std::span<const uint8_t> comment,
                                   std::span<const uint8_t> setup);

}
