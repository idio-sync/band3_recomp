#include "skin_images.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <map>
#include "src/Net/milo.h"

namespace band3::launcher::ui {

namespace {

constexpr SkinSource kSources[] = {
    {SkinPart::kHighlight, "ui/song_select/gen/song_select.milo_xbox", "song_highlight_bg.bmp"},
    {SkinPart::kStrip, "ui/song_select/gen/song_select.milo_xbox", "header_greyscale.bmp"},
    {SkinPart::kFrame, "ui/overshell/gen/overshell_player_common.milo_xbox", "pause_container.bmp"},
    {SkinPart::kFill, "ui/global/gen/dialog_common.milo_xbox", "dialog_wallpaper.bmp"},
    {SkinPart::kLineFade, "ui/overshell/gen/overshell.milo_xbox", "line_fade.png"},
    {SkinPart::kGrain, "ui/endgame/gen/coop_endgame.milo_xbox", "wallpaper_coop_endgame.png"},
    // Rock Band 3 Deluxe's copy of the header's normal map is DXT; the game's
    // own (main_hub's) is BC5, which band3 doesn't decode yet
    {SkinPart::kStreaks, "dx/custom_textures/overshell/original/gen/header_song_bg_normal.png_xbox", ""},
    {SkinPart::kStreaks, "ui/main/gen/main_hub.milo_xbox", "header_song_bg_normal.png"},
    {SkinPart::kIcons, "ui/track/gen/track_shared.milo_xbox", "rb3_icons.bmp"},
};

std::optional<content::ArkArchive> OpenArchive(const std::filesystem::path& header) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(header, ec)) return std::nullopt;
    std::string error;
    return content::ArkArchive::Open(header, error);
}

// a box blur of `radius` along x (step 1) or y (step w) of a w x h map
std::vector<float> BoxBlur(const std::vector<float>& in, int w, int h, int radius, bool along_x) {
    std::vector<float> out(in.size());
    const int n = along_x ? w : h;
    for (int line = 0; line < (along_x ? h : w); line++) {
        const auto at = [&](int i) -> float {
            i = std::clamp(i, 0, n - 1);
            return along_x ? in[size_t(line) * w + i] : in[size_t(i) * w + line];
        };
        float sum = 0;
        for (int i = -radius; i <= radius; i++) sum += at(i);
        for (int i = 0; i < n; i++) {
            (along_x ? out[size_t(line) * w + i] : out[size_t(i) * w + line]) = sum / (2 * radius + 1);
            sum += at(i + radius + 1) - at(i - radius);
        }
    }
    return out;
}

}

std::span<const SkinSource> SkinSources() { return kSources; }

GameFiles::GameFiles(const std::filesystem::path& root)
    : root_(root),
      patch_(OpenArchive(root / "gen" / "patch_xbox.hdr")),
      main_(OpenArchive(root / "gen" / "main_xbox.hdr")) {}

std::optional<std::string> GameFiles::Read(std::string_view path) const {
    std::error_code ec;
    const std::filesystem::path loose = root_ / std::filesystem::path(path);
    if (std::filesystem::is_regular_file(loose, ec)) {
        std::ifstream in(loose, std::ios::binary);
        if (in) return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    for (const auto* archive : {&patch_, &main_}) {
        if (!*archive || !(*archive)->Has(path)) continue;
        std::vector<uint8_t> bytes;
        std::string error;
        if ((*archive)->Read(path, bytes, error)) return std::string(bytes.begin(), bytes.end());
    }
    return std::nullopt;
}

SkinImages LoadSkinImages(const GameFiles& files) {
    SkinImages images;
    // each milo decompressed once, however many parts it has
    std::map<std::string_view, std::optional<std::string>> milos;
    for (const SkinSource& source : kSources) {
        auto& image = images[static_cast<size_t>(source.part)];
        if (image) continue;
        std::optional<http::Image> decoded;
        if (source.texture.empty()) {
            if (const auto file = files.Read(source.file)) decoded = http::DecodeXboxBitmap(*file);
        } else {
            auto found = milos.find(source.file);
            if (found == milos.end()) {
                const auto file = files.Read(source.file);
                found = milos.emplace(source.file, file ? milo::Decompress(*file) : std::nullopt).first;
            }
            if (found->second) decoded = milo::FindNamedBitmap(*found->second, source.texture);
        }
        if (!decoded || decoded->width <= 0 || decoded->height <= 0) continue;
        image = source.part == SkinPart::kStreaks ? MakeStreaks(*decoded) : std::move(*decoded);
    }
    return images;
}

http::Image MakeStreaks(const http::Image& normal_map) {
    constexpr int kStretch = 4;
    const int w = normal_map.width * kStretch, h = normal_map.height;
    http::Image out{w, h, std::vector<uint8_t>(size_t(w) * h * 4, 0)};
    if (w <= 0 || h <= 0) return out;
    // the up-facing slope at each output pixel, the source sampled across
    std::vector<float> slope(size_t(w) * h);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const float sx = (x + 0.5f) / kStretch - 0.5f;
            const int x0 = std::clamp(int(std::floor(sx)), 0, normal_map.width - 1);
            const int x1 = std::min(x0 + 1, normal_map.width - 1);
            const float t = std::clamp(sx - x0, 0.0f, 1.0f);
            const auto green = [&](int px) {
                return float(normal_map.rgba[(size_t(y) * normal_map.width + px) * 4 + 1]);
            };
            const float g = green(x0) * (1 - t) + green(x1) * t;
            slope[size_t(y) * w + x] = std::max(0.0f, (g - 128.0f) / 127.0f);
        }
    }
    // softened: long along the streaks, a little across them
    slope = BoxBlur(BoxBlur(slope, w, h, 6, true), w, h, 1, false);
    for (size_t i = 0; i < slope.size(); i++) {
        const float a = std::clamp(std::pow(slope[i], 0.8f) * 1.4f, 0.0f, 1.0f);
        out.rgba[i * 4 + 0] = 40;
        out.rgba[i * 4 + 1] = 70;
        out.rgba[i * 4 + 2] = 210;
        out.rgba[i * 4 + 3] = uint8_t(std::lround(a * 255));
    }
    return out;
}

}
