#pragma once
#include <array>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <imgui.h>
#include <rex/ui/immediate_drawer.h>
#include "skin_images.h"

// The launcher's RB3 textures (skin_images.h) as textures ImGui draws: read
// and decoded on a thread of their own, so the launcher shows at once and
// fills in as they come, then made textures on the UI thread. A part that
// isn't there (yet, or at all) draws as the theme's flat colours instead
// (widgets.h asks Has). Also the textures the look makes itself: a soft
// radial glow for the background's light and vignette.

namespace band3::launcher::ui {

class Skin {
public:
    using DrawerGetter = std::function<rex::ui::ImmediateDrawer*()>;

    explicit Skin(DrawerGetter drawer);
    ~Skin();
    Skin(const Skin&) = delete;
    Skin& operator=(const Skin&) = delete;

    // starts reading this game data folder's textures, or does nothing if
    // it's the one already read; the old ones stay until the new ones are in
    void Load(const std::filesystem::path& game_data_root);
    // once a frame on the UI thread, before drawing: makes textures of what
    // the thread has read
    void Poll();

    bool Has(SkinPart part) const { return textures_[Index(part)] != nullptr; }
    ImTextureRef Texture(SkinPart part) const { return Ref(textures_[Index(part)].get()); }
    // in texels
    ImVec2 Size(SkinPart part) const;

    // a white disc fading to clear at its edge, for glows and vignettes
    bool HasGlow() const { return glow_ != nullptr; }
    ImTextureRef Glow() const { return Ref(glow_.get()); }

private:
    static size_t Index(SkinPart part) { return static_cast<size_t>(part); }
    static ImTextureRef Ref(rex::ui::ImmediateTexture* texture);

    DrawerGetter drawer_;
    std::array<std::unique_ptr<rex::ui::ImmediateTexture>, kSkinPartCount> textures_;
    std::unique_ptr<rex::ui::ImmediateTexture> glow_;

    // the folder asked for, and what the thread has read from it
    std::filesystem::path root_;
    bool started_ = false;
    std::thread loader_;
    std::mutex mutex_;
    std::optional<SkinImages> loaded_;
};

}
