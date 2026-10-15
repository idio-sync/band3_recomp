#pragma once
#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <imgui.h>
#include "screen_kit.h"
#include "skin.h"
#include "src/Launcher/folder_pick.h"

// Find Rock Band 3: where band3 looks for the player's copy of the game. It
// searches this PC's drives as it opens (game_search.h) and lists the copies
// it finds, then the drives to browse folder by folder from a controller;
// folders holding the game say so. A uses one. The system's folder window is
// a row of its own, for the mouse and keyboard.

namespace band3::launcher::ui {

struct FindGameHost {
    // a folder holding the game was picked
    std::function<void(const std::filesystem::path&)> use;
    std::function<void()> back;
};

class FindGameScreen {
public:
    FindGameScreen(Skin& skin, FindGameHost host);
    ~FindGameScreen();

    // shown: searches this PC again, and starts at the top
    void Show();
    void Draw(ImGuiIO& io, std::span<const NavPad> pads);

    // what the search shares with its thread, which may outlive the screen
    struct Search;

private:
    struct Item {
        enum Kind { kBand, kFound, kDrive, kUp, kFolder, kChoose } kind;
        std::string label;
        std::filesystem::path path;
        bool game = false;
    };
    void Handle(Action action);
    void Activate(const Item& item);
    void Open(std::optional<std::filesystem::path> folder);
    void StartSearch();
    std::vector<Item> Items() const;

    Skin& skin_;
    FindGameHost host_;
    ScreenInput input_;
    FolderPicker picker_;
    std::shared_ptr<Search> search_;
    // the folder being browsed; none at the top (what was found, and the drives)
    std::optional<std::filesystem::path> folder_;
    // its folders, and which hold the game, as listed when it was opened
    std::vector<std::pair<std::filesystem::path, bool>> listing_;
    std::vector<std::filesystem::path> roots_;
    // among the items that can be picked (not the bands)
    int selected_ = 0;
    float scroll_ = 0;
    bool reveal_ = false;
    std::string problem_;
};

}
