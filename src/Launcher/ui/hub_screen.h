#pragma once
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <imgui.h>
#include "box.h"
#include "hub_model.h"
#include "screen_kit.h"
#include "skin.h"

// The launcher's first screen, after Rock Band 3's main menu: BAND3 over the
// menu (Play first, or Find Rock Band 3 first while the game can't be found)
// and a status card for the player: whether Rock Band 3 is found and where,
// Deluxe's version, how many songs, the controllers plugged in, and the last
// song played. Start plays from here as from every launcher screen.

namespace band3::launcher::ui {

// a controller for the card's icons
struct HubController {
    std::optional<uint8_t> sub_type;
    // a MIDI drum kit or keyboard, whose kind says what it is
    bool midi_drums = false;
    bool midi_keys = false;
};

struct HubHost {
    std::function<void(HubItem)> pick;
    // the game data folder the game would start with, and whether it holds the game
    std::function<std::filesystem::path()> game_data_root;
    std::function<bool()> game_found;
    // what's plugged in, for the card
    std::function<std::vector<HubController>()> controllers;
    // where the score history is
    std::function<std::filesystem::path()> user_data_root;
};

class HubScreen {
public:
    HubScreen(Skin& skin, HubHost host);
    ~HubScreen();

    // shown again: the first item selected, and the press that showed it isn't one
    void Show();
    void Draw(ImGuiIO& io, std::span<const NavPad> pads);
    void ShowBox(Box box);

    // what the card reads off the disk, on a thread of its own
    struct Facts;

private:
    void Handle(Action action);
    void Pick(int index);
    void DrawCard(ImDrawList* dl, const Page& page, bool found);

    Skin& skin_;
    HubHost host_;
    ScreenInput input_;
    int selected_ = 0;
    std::optional<Box> box_;
    int box_frame_ = -1;
    std::shared_ptr<Facts> facts_;
    std::filesystem::path facts_root_;
    bool facts_started_ = false;
    std::vector<HubEntry> menu_;
};

}
