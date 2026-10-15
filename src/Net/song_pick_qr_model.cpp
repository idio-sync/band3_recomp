#include "song_pick_qr_model.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include "src/Render/present_model.h"
#include "src/ThirdParty/qrcodegen/qrcodegen.hpp"

namespace band3::song_pick_qr {

std::string PageUrl(std::string_view listen_address, std::string_view local_address, int port) {
    const std::string_view host = listen_address == "0.0.0.0" ? local_address : listen_address;
    if (host.empty() || host.starts_with("127.") || port <= 0) return {};
    return "http://" + std::string(host) + ":" + std::to_string(port) + "/";
}

std::string Caption(std::string_view url) {
    if (const size_t scheme = url.find("://"); scheme != std::string_view::npos) {
        url.remove_prefix(scheme + 3);
    }
    while (url.ends_with('/')) url.remove_suffix(1);
    std::string out(url);
    for (char& c : out) c = char(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::optional<Modules> Encode(const std::string& text, int quiet) {
    try {
        const auto qr = qrcodegen::QrCode::encodeText(text.c_str(), qrcodegen::QrCode::Ecc::MEDIUM);
        Modules m;
        m.size = qr.getSize() + quiet * 2;
        m.dark.assign(size_t(m.size) * size_t(m.size), false);
        for (int y = 0; y < qr.getSize(); y++) {
            for (int x = 0; x < qr.getSize(); x++) {
                m.dark[size_t(y + quiet) * size_t(m.size) + size_t(x + quiet)] = qr.getModule(x, y);
            }
        }
        return m;
    } catch (const qrcodegen::data_too_long&) {
        return std::nullopt;
    }
}

Rect GameFrame(uint32_t window_w, uint32_t window_h, bool letterbox) {
    const render::ImageRect r = render::LetterboxRect(window_w, window_h, letterbox);
    return {float(r.x), float(r.y), float(r.w), float(r.h)};
}

int ModulePixels(int modules, float scale) {
    if (modules <= 0) return 1;
    return std::max(1, int(std::lround(kCodeSize * scale / float(modules))));
}

Line Layout(const milo::Font& font, std::string_view text, float px, int texture_w, int texture_h) {
    Line line;
    if (font.cell_h <= 0 || px <= 0) return line;
    std::unordered_map<uint16_t, const milo::Glyph*> glyphs;
    for (const milo::Glyph& g : font.glyphs) glyphs.emplace(g.code, &g);
    std::unordered_map<uint32_t, float> kerning;
    for (const milo::Kern& k : font.kerning) kerning[uint32_t(k.left) | uint32_t(k.right) << 16] = k.kerning;

    const float scale = px / font.cell_h;
    const float cell = font.cell_w * scale;
    float x = 0;
    std::optional<uint16_t> previous;
    for (const char c : text) {
        const uint16_t code = static_cast<unsigned char>(c);
        const auto found = glyphs.find(code);
        if (found == glyphs.end()) {
            x += px / 3;
            previous.reset();
            continue;
        }
        const milo::Glyph& g = *found->second;
        if (previous) {
            if (const auto k = kerning.find(uint32_t(*previous) | uint32_t(code) << 16); k != kerning.end()) {
                x += k->second * cell;
            }
        }
        line.glyphs.push_back({g.u * float(texture_w), g.v * float(texture_h), g.width * font.cell_w,
                               font.cell_h, x, g.width * cell});
        x += (g.advance + font.base_kerning) * cell;
        previous = code;
    }
    line.width = std::max(0.0f, x);
    return line;
}

Mask Rasterize(const Line& line, const http::Image& texture, int height) {
    Mask mask;
    if (height <= 0 || line.width <= 0) return mask;
    mask.width = int(std::ceil(line.width));
    mask.height = height;
    mask.alpha.assign(size_t(mask.width) * size_t(height), 0);
    auto texel = [&](float sx, float sy) -> float {
        const int tx = std::clamp(int(sx), 0, texture.width - 1);
        const int ty = std::clamp(int(sy), 0, texture.height - 1);
        return texture.rgba[(size_t(ty) * size_t(texture.width) + size_t(tx)) * 4 + 3];
    };
    if (texture.width <= 0 || texture.height <= 0) return mask;
    for (const Placed& g : line.glyphs) {
        if (g.w <= 0) continue;
        // texture pixels per mask pixel, each way, and samples enough to
        // see every texel a mask pixel covers
        const float step_x = g.src_w / g.w, step_y = g.src_h / float(height);
        const int n = std::clamp(int(std::ceil(std::max(step_x, step_y))) + 1, 1, 16);
        const int x0 = std::max(0, int(std::floor(g.x)));
        const int x1 = std::min(mask.width, int(std::ceil(g.x + g.w)));
        for (int y = 0; y < height; y++) {
            for (int x = x0; x < x1; x++) {
                float sum = 0;
                for (int j = 0; j < n; j++) {
                    const float py = float(y) + (float(j) + 0.5f) / float(n);
                    for (int i = 0; i < n; i++) {
                        const float px = float(x) + (float(i) + 0.5f) / float(n);
                        // samples off the glyph count as clear
                        if (px < g.x || px >= g.x + g.w) continue;
                        sum += texel(g.src_x + (px - g.x) * step_x, g.src_y + py * step_y);
                    }
                }
                uint8_t& out = mask.alpha[size_t(y) * size_t(mask.width) + size_t(x)];
                // where kerning overlaps two glyphs, the stronger
                out = std::max(out, uint8_t(std::lround(sum / float(n * n))));
            }
        }
    }
    return mask;
}

}  // namespace band3::song_pick_qr
