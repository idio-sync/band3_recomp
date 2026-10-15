#pragma once
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <imgui.h>
#include "box.h"
#include "onscreen_keyboard.h"
#include "screen_kit.h"
#include "skin.h"
#include "src/Launcher/launcher_settings.h"
#include "src/Launcher/video_library.h"

// The Music Videos screen, laid out as Rock Band 3's Music Library: every
// song under letter bands with how its video stands (synced, not synced, no
// video, finding), action bands at the top to find videos for every song and
// to open the videos' settings, and the song picked in the panel: its video,
// its offset and how well it's synced. A song's options (play it, sync it
// again, search, choose a result, paste a link, choose a file, delete) open
// in a box; left and right move its offset; LB and RB jump by letter.

namespace band3::launcher::ui {

struct VideosHost {
    // the videos folders, as set now
    std::function<std::vector<std::filesystem::path>()> folders;
    std::function<void()> settings;
    std::function<void()> back;
    // Start: the launcher plays; none in game
    std::function<void()> play;
};

class VideosScreen {
public:
    VideosScreen(Skin& skin, VideosHost host);

    void Show();
    void Draw(ImGuiIO& io, std::span<const NavPad> pads, bool over_game);
    // B or Escape, from elsewhere (the pause menu's bind when it isn't Escape)
    void Back() { Handle(Action::kBack); }

private:
    enum class Filter { kAll, kWithout, kWith, kToReview, kFailed, kUnmatched };
    struct Item {
        enum Kind { kFindAll, kSettings, kFolder, kFilter, kLetter, kSong, kUnmatched, kEmpty } kind;
        std::string label;
        // into the library's songs or videos
        int index = -1;
        // a letter band's counts
        std::string right;
    };
    // what the cursor stops on: not the letter bands or a line of text
    static bool Pickable(Item::Kind kind) { return kind != Item::kLetter && kind != Item::kEmpty; }
    std::vector<Item> Items() const;
    void Handle(Action action);
    void Activate(const Item& item);
    void Step(const Item& item, int delta);
    // the song's options, in a box
    void Options(int song);
    void ChooseResult(int song);
    void FindFor(int song, video::VideoJob job);
    // a box saying yt-dlp can't be run; false when it can
    bool NeedsYtDlp();
    void ShowBox(Box box);
    void Jump(int direction);

    void DrawPanel(ImDrawList* dl, const Page& page, const Item* item);

    Skin& skin_;
    VideosHost host_;
    VideoLibrary library_;
    ScreenInput input_;
    Filter filter_ = Filter::kAll;
    // among the items that can be picked (not the letter bands)
    int selected_ = 0;
    float scroll_ = 0;
    bool reveal_ = false;
    std::optional<Box> box_;
    int box_frame_ = -1;
    // a link being typed for this song
    std::optional<int> typing_link_;
    OnScreenKeyboard keyboard_;
    // what the last sync, or anything else just done, came to: the strip says
    std::string note_;
    ImU32 note_color_ = 0;
    double note_until_ = 0;
    bool over_game_ = false;
};

}
