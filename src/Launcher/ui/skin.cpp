#include "skin.h"
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <cmath>
#include <utility>
#include <vector>

namespace band3::launcher::ui {

namespace {

// the parts drawn tiled
bool Repeats(SkinPart part) { return part == SkinPart::kGrain || part == SkinPart::kFill; }

constexpr int kGlowSize = 128;

std::vector<uint8_t> MakeGlow() {
    std::vector<uint8_t> rgba(size_t(kGlowSize) * kGlowSize * 4);
    for (int y = 0; y < kGlowSize; y++) {
        for (int x = 0; x < kGlowSize; x++) {
            const float dx = (x + 0.5f) / kGlowSize * 2 - 1, dy = (y + 0.5f) / kGlowSize * 2 - 1;
            const float d = std::min(1.0f, std::sqrt(dx * dx + dy * dy));
            // smooth from the middle to nothing at the edge
            const float a = (1 - d) * (1 - d) * (1 + 2 * d);
            uint8_t* p = &rgba[(size_t(y) * kGlowSize + x) * 4];
            p[0] = p[1] = p[2] = 255;
            p[3] = uint8_t(std::lround(a * 255));
        }
    }
    return rgba;
}

}

Skin::Skin(DrawerGetter drawer) : drawer_(std::move(drawer)) {}

Skin::~Skin() {
    if (loader_.joinable()) loader_.join();
}

ImTextureRef Skin::Ref(rex::ui::ImmediateTexture* texture) {
    return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<uintptr_t>(texture)));
}

ImVec2 Skin::Size(SkinPart part) const {
    const auto& t = textures_[Index(part)];
    return t ? ImVec2(float(t->width), float(t->height)) : ImVec2(0, 0);
}

void Skin::Load(const std::filesystem::path& game_data_root) {
    if (started_ && game_data_root == root_) return;
    started_ = true;
    // a folder changed while the last one is still read: that one finishes first
    if (loader_.joinable()) loader_.join();
    root_ = game_data_root;
    loader_ = std::thread([this, root = game_data_root] {
        SkinImages images = LoadSkinImages(GameFiles(root));
        size_t found = 0;
        for (const auto& image : images) found += image.has_value();
        REXLOG_INFO("Launcher: {} of {} RB3 textures from {}", found, kSkinPartCount,
                    rex::path_to_utf8(root));
        std::lock_guard lock(mutex_);
        loaded_ = std::move(images);
    });
}

void Skin::Poll() {
    rex::ui::ImmediateDrawer* drawer = drawer_ ? drawer_() : nullptr;
    if (!drawer) return;
    if (!glow_) {
        const std::vector<uint8_t> glow = MakeGlow();
        glow_ = drawer->CreateTexture(kGlowSize, kGlowSize, rex::ui::ImmediateTextureFilter::kLinear,
                                      false, glow.data());
    }
    std::optional<SkinImages> images;
    {
        std::lock_guard lock(mutex_);
        images = std::move(loaded_);
        loaded_.reset();
    }
    if (!images) return;
    if (loader_.joinable()) loader_.join();
    for (size_t i = 0; i < kSkinPartCount; i++) {
        const auto& image = (*images)[i];
        if (!image) {
            textures_[i].reset();
            continue;
        }
        textures_[i] = drawer->CreateTexture(uint32_t(image->width), uint32_t(image->height),
                                             rex::ui::ImmediateTextureFilter::kLinear,
                                             Repeats(static_cast<SkinPart>(i)), image->rgba.data());
    }
}

}
