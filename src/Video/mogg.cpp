#include "src/Video/mogg.h"

namespace band3::video {

namespace {

uint32_t Le32(std::span<const uint8_t> b, size_t at) {
    return uint32_t(b[at]) | uint32_t(b[at + 1]) << 8 | uint32_t(b[at + 2]) << 16 |
           uint32_t(b[at + 3]) << 24;
}

// an Ogg page's header before its lacing values
constexpr size_t kPageHeader = 27;
// its header_type's bit for a page that goes on with the last page's packet
constexpr uint8_t kContinued = 0x01;

}

std::optional<MoggHeader> ParseMoggHeader(std::span<const uint8_t> mogg) {
    if (mogg.size() < 8) return std::nullopt;
    MoggHeader h{Le32(mogg, 0), Le32(mogg, 4)};
    if (h.ogg_offset < 8 || h.ogg_offset > mogg.size()) return std::nullopt;
    return h;
}

bool OggPackets::NextPage() {
    while (at_ + kPageHeader <= ogg_.size()) {
        const std::span<const uint8_t> page = ogg_.subspan(at_);
        if (page[0] != 'O' || page[1] != 'g' || page[2] != 'g' || page[3] != 'S' || page[4] != 0)
            return false;
        const uint8_t type = page[5];
        const uint32_t serial = Le32(page, 14);
        const size_t segments = page[26];
        if (kPageHeader + segments > page.size()) return false;
        size_t body = 0;
        for (size_t i = 0; i < segments; i++) body += page[kPageHeader + i];
        if (kPageHeader + segments + body > page.size()) return false;
        at_ += kPageHeader + segments + body;
        if (!serial_) {
            serial_ = serial;
            skipping_ = (type & kContinued) != 0;
        } else if (serial != *serial_) {
            continue;
        }
        lacing_ = page.subspan(kPageHeader, segments);
        body_ = page.subspan(kPageHeader + segments, body);
        segment_ = 0;
        body_at_ = 0;
        return true;
    }
    return false;
}

bool OggPackets::Next(std::vector<uint8_t>& packet) {
    packet.clear();
    for (;;) {
        if (segment_ >= lacing_.size()) {
            if (!NextPage()) return false;
            continue;
        }
        const size_t n = lacing_[segment_++];
        if (!skipping_) packet.insert(packet.end(), body_.begin() + body_at_, body_.begin() + body_at_ + n);
        body_at_ += n;
        // a lacing value under 255 ends the packet
        if (n < 255) {
            if (skipping_) {
                skipping_ = false;
                continue;
            }
            return true;
        }
    }
}

std::vector<uint8_t> XiphExtradata(std::span<const uint8_t> id, std::span<const uint8_t> comment,
                                   std::span<const uint8_t> setup) {
    std::vector<uint8_t> out{2};
    for (size_t size : {id.size(), comment.size()}) {
        for (; size >= 255; size -= 255) out.push_back(255);
        out.push_back(uint8_t(size));
    }
    out.insert(out.end(), id.begin(), id.end());
    out.insert(out.end(), comment.begin(), comment.end());
    out.insert(out.end(), setup.begin(), setup.end());
    return out;
}

}
