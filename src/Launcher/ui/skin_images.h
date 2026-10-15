#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include "src/Content/ark_archive.h"
#include "src/Net/album_art.h"

// The launcher's RB3 look, read from the player's own game data: the textures
// of the game's menus (song_select's highlight bar, the pause container, ...)
// decoded to RGBA. Nothing here needs the game or ImGui; skin.h makes them
// textures. Any of them may be missing (no game data yet, a texture in a
// format band3 doesn't decode), and the launcher draws flat colours in its
// place.

namespace band3::launcher::ui {

enum class SkinPart : uint8_t {
    kHighlight,  // the yellow selection bar
    kStrip,      // the grey strip under a screen's title
    kFrame,      // the pause container: panels', boxes' and the pause menu's frame
    kFill,       // a panel's wallpaper, under a dark wash
    kLineFade,   // the fading rule above the help bar and under headings
    kGrain,      // the background's embossed ornament
    kStreaks,    // the background's blue streaks (MakeStreaks)
    kIcons,      // RB3's instrument icons, 4x4 cells
    kCount,
};
inline constexpr size_t kSkinPartCount = static_cast<size_t>(SkinPart::kCount);

// Where a part comes from: a texture named `texture` inside the milo `file`,
// or, with no `texture`, `file` itself, a standalone .png_xbox. Parts with
// more than one source take the first that decodes.
struct SkinSource {
    SkinPart part;
    std::string_view file;
    std::string_view texture;
};
std::span<const SkinSource> SkinSources();

// The game data's files as the game finds them: loose in the folder first,
// then in the title update's archive (gen/patch_xbox.hdr), then the main one
// (gen/main_xbox.hdr).
class GameFiles {
public:
    explicit GameFiles(const std::filesystem::path& root);
    // a file's bytes; nullopt when none of the three has it
    std::optional<std::string> Read(std::string_view path) const;
    // whether either archive opened
    bool HasArchives() const { return patch_.has_value() || main_.has_value(); }

private:
    std::filesystem::path root_;
    std::optional<content::ArkArchive> patch_;
    std::optional<content::ArkArchive> main_;
};

using SkinImages = std::array<std::optional<http::Image>, kSkinPartCount>;

// every part that can be read and decoded; the streaks made from the header's
// normal map (MakeStreaks)
SkinImages LoadSkinImages(const GameFiles& files);

// The background's streaks, as the Music Library's header shows them: the
// game lights header_song_bg's normal map with moving lights, which the
// up-facing slopes (the green channel above its middle) catch. Those slopes,
// stretched four times wider and softened, as a blue that's opaque where they
// are steepest and clear where the map is flat.
http::Image MakeStreaks(const http::Image& normal_map);

}
