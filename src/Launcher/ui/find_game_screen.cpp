#include "find_game_screen.h"
#include <algorithm>
#include <format>
#include <mutex>
#include <thread>
#include <rex/filesystem.h>
#include "src/Launcher/game_data_check.h"
#include "src/Launcher/game_search.h"
#include "src/Launcher/launcher_style.h"
#include "hub_model.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace fs = std::filesystem;

struct FindGameScreen::Search {
    std::atomic<bool> stop{false};
    // how many folders it has looked in so far
    std::atomic<size_t> looked_in{0};
    std::mutex mutex;
    bool done = false;
    std::vector<fs::path> found;
};

namespace {

constexpr float kLeft = 40, kRight = 890, kTop = 140, kBottom = 652;
constexpr float kBandHeight = 34, kRowPitch = 41, kRowHeight = 40, kGap = 6;

std::string Shown(const fs::path& path) { return rex::path_to_utf8(fs::path(path).make_preferred()); }

}

FindGameScreen::FindGameScreen(Skin& skin, FindGameHost host) : skin_(skin), host_(std::move(host)) {}

FindGameScreen::~FindGameScreen() {
    if (search_) search_->stop = true;
}

void FindGameScreen::Show() {
    input_.Show();
    folder_.reset();
    selected_ = 0;
    scroll_ = 0;
    problem_.clear();
    roots_ = SearchRoots();
    root_names_.clear();
    for (const fs::path& root : roots_) root_names_.push_back(DescribeRoot(root));
    StartSearch();
}

void FindGameScreen::StartSearch() {
    if (search_) search_->stop = true;
    search_ = std::make_shared<Search>();
    std::thread([search = search_, roots = roots_] {
        std::vector<fs::path> found = FindGameFolders(roots, SearchLimits{}, &search->stop, &search->looked_in);
        std::lock_guard lock(search->mutex);
        search->found = std::move(found);
        search->done = true;
    }).detach();
}

void FindGameScreen::Open(std::optional<fs::path> folder) {
    folder_ = std::move(folder);
    listing_.clear();
    if (folder_) {
        for (fs::path& child : ListFolders(*folder_)) {
            const bool game = HoldsGame(child);
            listing_.emplace_back(std::move(child), game);
        }
    }
    selected_ = 0;
    scroll_ = 0;
    reveal_ = true;
}

std::vector<FindGameScreen::Item> FindGameScreen::Items() const {
    std::vector<Item> out;
    if (folder_) {
        out.push_back({Item::kBand, Shown(*folder_)});
        out.push_back({Item::kUp, "Up a level"});
        for (const auto& [path, game] : listing_) {
            out.push_back({Item::kFolder, rex::path_to_utf8(path.filename()), path, game});
        }
        if (listing_.empty()) out.push_back({Item::kNote, "No folders in here"});
        return out;
    }
    std::vector<fs::path> found;
    bool done = false;
    {
        std::lock_guard lock(search_->mutex);
        found = search_->found;
        done = search_->done;
    }
    out.push_back({Item::kBand, done ? "Found on this PC" : "Searching this PC..."});
    for (const fs::path& path : found) out.push_back({Item::kFound, Shown(path), path, true});
    if (done && found.empty()) out.push_back({Item::kNote, "Not found by itself: open the drive you copied it to, or choose a folder in a window"});
    out.push_back({Item::kBand, "Browse"});
    for (size_t i = 0; i < roots_.size(); i++) {
        out.push_back({Item::kDrive, i < root_names_.size() ? root_names_[i] : Shown(roots_[i]), roots_[i]});
    }
    out.push_back({Item::kChoose, "Choose a folder in a window..."});
    return out;
}

void FindGameScreen::Activate(const Item& item) {
    switch (item.kind) {
    case Item::kFound:
        if (host_.use) host_.use(item.path);
        break;
    case Item::kFolder:
        if (item.game) {
            if (host_.use) host_.use(item.path);
        } else {
            Open(item.path);
        }
        break;
    case Item::kDrive: Open(item.path); break;
    case Item::kUp: {
        const fs::path parent = folder_->parent_path();
        // a drive's root is its own parent: back to the top
        if (parent == *folder_ || std::ranges::find(roots_, *folder_) != roots_.end()) {
            Open(std::nullopt);
        } else {
            Open(parent);
        }
        break;
    }
    case Item::kChoose: picker_.Start("game_data_root", folder_ ? rex::path_to_utf8(*folder_) : std::string()); break;
    case Item::kBand:
    case Item::kNote: break;
    }
}

void FindGameScreen::Handle(Action action) {
    std::vector<Item> items = Items();
    std::vector<const Item*> pickable;
    for (const Item& i : items) {
        if (i.kind != Item::kBand && i.kind != Item::kNote) pickable.push_back(&i);
    }
    const int count = int(pickable.size());
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        selected_ = MoveSelection(selected_, action == Action::kUp ? -1 : 1, count, false);
        reveal_ = true;
        break;
    case Action::kSelect:
        if (selected_ < count) Activate(*pickable[size_t(selected_)]);
        break;
    case Action::kBack:
        if (folder_) {
            Activate(Item{Item::kUp, ""});
        } else if (host_.back) {
            host_.back();
        }
        break;
    case Action::kReset:
        Open(std::nullopt);
        StartSearch();
        break;
    default: break;
    }
}

void FindGameScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads) {
    if (!search_) Show();
    if (const auto answer = picker_.Take()) {
        if (!answer->error.empty()) {
            problem_ = "The folder window didn't open: " + answer->error;
        } else if (answer->folder) {
            const fs::path folder(std::u8string(answer->folder->begin(), answer->folder->end()));
            if (CheckGameData(folder).ok) {
                if (host_.use) host_.use(folder);
                return;
            }
            problem_ = "No Rock Band 3 in " + *answer->folder + ".";
            Open(folder);
        }
    }
    for (const Action a : input_.Read(pads)) Handle(a);

    const std::vector<Item> items = Items();
    std::vector<int> pickable;
    for (int i = 0; i < int(items.size()); i++) {
        if (items[size_t(i)].kind != Item::kBand && items[size_t(i)].kind != Item::kNote) pickable.push_back(i);
    }
    selected_ = std::clamp(selected_, 0, std::max(0, int(pickable.size()) - 1));
    const Item* current = pickable.empty() ? nullptr : &items[size_t(pickable[size_t(selected_)])];

    const Page page = PageFor(io);
    ImDrawList* dl = BeginScreen("band3##find_game", io, skin_);
    DrawTitle(dl, page.At(70, 14), "Find Rock Band 3");
    DrawStrip(dl, page.At(0, 96), page.At(890, 132), skin_);
    std::string strip;
    if (folder_) {
        strip = "Looking in " + Shown(*folder_);
    } else {
        std::lock_guard lock(search_->mutex);
        strip = !search_->done ? "Searching this PC... " + Thousands(search_->looked_in.load()) + " folders"
                               : search_->found.empty() ? std::format("Searched {} {}: no copy found", roots_.size(), roots_.size() == 1 ? "drive" : "drives")
                               : "Searched this PC: " + std::to_string(search_->found.size()) +
                                     (search_->found.size() == 1 ? " copy found" : " copies found");
    }
    PaintText(dl, page.At(40, 100), kCaps(text::kStrip), color::kStripText, strip, Px(840));

    // the layout, then the rows in sight
    std::vector<float> ys;
    float y = 0;
    for (size_t i = 0; i < items.size(); i++) {
        if (items[i].kind == Item::kBand) {
            if (i > 0) y += kGap;
            ys.push_back(y);
            y += kBandHeight;
        } else {
            ys.push_back(y);
            y += kRowPitch;
        }
    }
    const float area = kBottom - kTop;
    if (reveal_ && current) {
        const float sy = ys[size_t(pickable[size_t(selected_)])];
        if (sy < scroll_ + kBandHeight) scroll_ = std::max(0.0f, sy - kBandHeight);
        if (sy + kRowHeight > scroll_ + area) scroll_ = sy + kRowHeight - area;
        reveal_ = false;
    }
    const ImVec2 amin = page.At(kLeft, kTop), amax = page.At(kRight, kBottom);
    if (ImGui::IsMouseHoveringRect(amin, amax) && io.MouseWheel != 0) scroll_ -= io.MouseWheel * kRowPitch * 3;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, y - area));
    dl->PushClipRect(amin, amax, true);
    int pick = 0;
    for (size_t i = 0; i < items.size(); i++) {
        const Item& item = items[i];
        const float top = kTop + ys[i] - scroll_;
        if (item.kind == Item::kBand) {
            DrawBand(dl, page.At(kLeft, top), page.At(kRight, top + kBandHeight), item.label, BandKind::kSection, skin_);
            continue;
        }
        if (item.kind == Item::kNote) {
            PaintText(dl, page.At(kLeft + 20, top + 9), kPlain(text::kBody), color::kMuted, item.label);
            continue;
        }
        const int index = pick++;
        if (top > kBottom || top + kRowHeight < kTop) continue;
        RowLook look;
        look.label = item.label;
        look.selected = index == selected_;
        look.value_caps = true;
        look.value = item.game ? "Game found" : item.kind == Item::kUp ? "" : "Open";
        look.action = !item.game && item.kind != Item::kUp;
        look.on = item.game;
        const ImVec2 min = page.At(kLeft, top), max = page.At(kRight, top + kRowHeight);
        DrawRow(dl, min, max, look, skin_);
        if (ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseHoveringRect(amin, amax)) {
            if (input_.MouseMoved()) selected_ = index;
            if (input_.Clicked()) {
                selected_ = index;
                Activate(item);
            }
        }
    }
    dl->PopClipRect();

    // the panel: what the selected folder is
    DrawPanel(dl, page.At(906, 30), page.At(1256, 650), skin_);
    const float px = 932, pw = 298;
    float py = 56;
    const auto para = [&](std::string_view text, ImU32 color) {
        for (const std::string& line : WrapText(kPlain(text::kBody), text, Px(pw))) {
            PaintText(dl, page.At(px, py), kPlain(text::kBody), color, line);
            py += text::kBodyLine;
        }
    };
    const auto heading = [&](std::string_view text) {
        for (const std::string& line : WrapText(kCaps(text::kPanelHeading), text, Px(pw))) {
            PaintText(dl, page.At(px, py), kCaps(text::kPanelHeading), color::kText, line);
            py += text::kPanelHeading + 4;
        }
        DrawRule(dl, page.At(px, py + 6), page.At(px + pw, py + 10), skin_);
        py += 22;
    };
    const ImU32 body = color::kBody;
    if (current && current->game) {
        heading("Rock Band 3 found");
        para(Shown(current->path), color::kMuted);
        py += 10;
        para("Select it to use this folder. band3 remembers it and checks it again each time it starts.", body);
    } else if (current && current->kind == Item::kUp) {
        heading("Up a level");
        para("Back to the folder this one is in.", body);
    } else if (current && current->kind == Item::kChoose) {
        heading("Choose a folder");
        para("Opens the system's folder window, to pick the game's folder with the mouse and keyboard.", body);
    } else if (current) {
        heading(current->label);
        para("Open folders until one says Game found.", body);
        py += 8;
        para("Rock Band 3 isn't in this folder itself. The game's folder has a folder named gen in it, holding "
             "main_xbox.hdr.",
             color::kMuted);
    } else {
        heading("Find Rock Band 3");
        para("band3 plays your own copy of Rock Band 3, copied from your Xbox 360 disc to this PC.", body);
    }
    if (!problem_.empty()) {
        py += 12;
        para(problem_, color::kBad);
    }

    const bool use = current && current->game;
    const HelpItem help[] = {{HelpButton::kA, use ? "Use this folder" : "Open"},
                             {HelpButton::kB, folder_ ? std::string("Up a level")
                                                      : host_.back_label ? host_.back_label() : std::string("Back")},
                             {HelpButton::kY, "Search again"}};
    const int hovered = DrawHelpBar(dl, ImVec2(0, page.At(0, 664).y), io.DisplaySize, help, input_.Keyboard(), false,
                                    skin_);
    if (hovered >= 0 && input_.Clicked()) {
        Handle(hovered == 0 ? Action::kSelect : hovered == 1 ? Action::kBack : Action::kReset);
    }
    EndScreen();
}

}
