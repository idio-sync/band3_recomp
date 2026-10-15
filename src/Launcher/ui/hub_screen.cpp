#include "hub_screen.h"
#include <algorithm>
#include <cmath>
#include <fstream>
#include <mutex>
#include <set>
#include <thread>
#include <rex/filesystem.h>
#include "src/Content/ark_archive.h"
#include "src/Content/game_song_list.h"
#include "src/Content/package_songs.h"
#include "src/Launcher/launcher_style.h"
#include "skin_images.h"
#include "widgets.h"

namespace band3::launcher::ui {

struct HubScreen::Facts {
    std::mutex mutex;
    bool done = false;
    std::optional<std::string> deluxe;
    int songs = 0;
    std::optional<Played> last;
};

namespace {

// the score history's last few lines
std::string HistoryTail(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return {};
    const std::streamoff size = in.tellg();
    const std::streamoff start = std::max<std::streamoff>(0, size - 16384);
    in.seekg(start);
    std::string tail(size_t(size - start), '\0');
    in.read(tail.data(), std::streamsize(tail.size()));
    return tail;
}

// 1,612
std::string Thousands(int n) {
    std::string digits = std::to_string(n);
    for (int i = int(digits.size()) - 3; i > 0; i -= 3) digits.insert(size_t(i), ",");
    return digits;
}

// the card's facts, read off the disk
void ReadFacts(const std::shared_ptr<HubScreen::Facts>& facts, std::filesystem::path root,
               std::filesystem::path user_data) {
    std::optional<std::string> deluxe;
    if (const auto dtb = GameFiles(root).Read("dx/locale/gen/dx_version.dtb")) {
        const auto bytes = std::span(reinterpret_cast<const uint8_t*>(dtb->data()), dtb->size());
        deluxe = DeluxeVersion(content::DecryptDtb(bytes));
    }
    // the songs the game listed when it last ran, and band3's packages', once each
    std::set<std::string> shortnames;
    auto add = [&](std::string name) {
        for (char& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        shortnames.insert(std::move(name));
    };
    for (const content::ListedSong& s : content::LastGameSongs()) add(s.shortname);
    for (const content::PackageSongs& p : content::PackageSongsNow(std::chrono::milliseconds(0))) {
        for (const content::DtaSong& s : p.songs) add(s.shortname);
    }
    const auto last = LastPlayed(HistoryTail(user_data / "score_history.jsonl"));
    std::lock_guard lock(facts->mutex);
    facts->deluxe = deluxe;
    facts->songs = int(shortnames.size());
    facts->last = last;
    facts->done = true;
}

constexpr ImU32 kCardText = IM_COL32(242, 242, 242, 255);
constexpr ImU32 kCardMuted = IM_COL32(163, 170, 187, 255);

}

HubScreen::HubScreen(Skin& skin, HubHost host) : skin_(skin), host_(std::move(host)) {}

HubScreen::~HubScreen() = default;

void HubScreen::Show() {
    selected_ = 0;
    box_.reset();
    input_.Show();
}

void HubScreen::ShowBox(Box box) {
    box_ = std::move(box);
    box_frame_ = ImGui::GetFrameCount();
}

void HubScreen::Handle(Action action) {
    if (box_) {
        if (const auto picked = StepBox(*box_, action)) {
            const auto chosen = std::move(box_->chosen);
            box_.reset();
            if (chosen) chosen(*picked);
        }
        return;
    }
    const int count = int(menu_.size());
    switch (action) {
    case Action::kUp:
    case Action::kDown: selected_ = MoveSelection(selected_, action == Action::kUp ? -1 : 1, count, true); break;
    case Action::kSelect: Pick(selected_); break;
    // B goes to Quit, as RB3's main menu does; it doesn't quit
    case Action::kBack: selected_ = count - 1; break;
    case Action::kPlay:
        if (host_.pick) host_.pick(HubItem::kPlay);
        break;
    default: break;
    }
}

void HubScreen::Pick(int index) {
    if (index < 0 || index >= int(menu_.size()) || !host_.pick) return;
    host_.pick(menu_[size_t(index)].item);
}

void HubScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads) {
    const bool found = host_.game_found ? host_.game_found() : true;
    menu_ = HubMenu(found);
    selected_ = std::clamp(selected_, 0, int(menu_.size()) - 1);

    // the card's facts, again when the game data folder changes
    const std::filesystem::path root = host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (!facts_started_ || root != facts_root_) {
        facts_started_ = true;
        facts_root_ = root;
        facts_ = std::make_shared<Facts>();
        std::thread(ReadFacts, facts_, root,
                    host_.user_data_root ? host_.user_data_root() : std::filesystem::path())
            .detach();
    }

    for (const Action a : input_.Read(pads)) Handle(a);

    const Page page = PageFor(io);
    ImDrawList* dl = BeginScreen("band3##hub", io, skin_);
    DrawTitle(dl, page.At(70, 18), "band3", text::kHubTitle);
    DrawStrip(dl, page.At(0, 132), page.At(770, 166), skin_);
    PaintText(dl, page.At(96, 138), kCaps(text::kStrip), IM_COL32(232, 232, 236, 255), "Rock Band 3 for PC");

    // the menu
    const TextStyle item_style = kCaps(text::kHubMenu);
    const float top = found ? 212 : 200;
    for (int i = 0; i < int(menu_.size()); i++) {
        const HubEntry& e = menu_[size_t(i)];
        const std::string_view name = HubItemName(e.item);
        const float y = top + 54.0f * i;
        const float w = TextWidth(item_style, name) / page.scale;
        const ImVec2 min = page.At(62, y), max = page.At(76 + w + 14, y + 52);
        const bool selected = i == selected_ && !box_;
        if (selected) DrawHighlight(dl, min, max, skin_, Px(4));
        ImU32 col = selected ? color::kOnYellow : color::kText;
        if (!e.enabled && !selected) col = IM_COL32(242, 242, 242, 97);
        PaintText(dl, page.At(76, y + 1), item_style, col, name);
        if (!box_ && ImGui::IsMouseHoveringRect(min, max)) {
            if (input_.MouseMoved()) selected_ = i;
            if (input_.Clicked()) Pick(i);
        }
    }

    // a tip under the menu
    if (found) {
        const float y = 505;
        dl->AddRectFilled(page.At(76, y + 5), page.At(98, y + 13), IM_COL32(185, 184, 29, 255));
        PaintText(dl, page.At(108, y), kCaps(22), color::kText, "Tip");
        float ty = y + 28;
        for (const std::string& line :
             WrapText(kPlain(text::kBody),
                      "In game, press Escape or hold both stick clicks for a second for the pause "
                      "menu and these settings.",
                      Px(600))) {
            PaintText(dl, page.At(108, ty), kPlain(text::kBody), IM_COL32(195, 199, 210, 255), line);
            ty += text::kBodyLine;
        }
    }

    DrawCard(dl, page, found);

    const HelpItem help[] = {{HelpButton::kA, "Select"}, {HelpButton::kB, "Go to Quit"}};
    const ImVec2 bar_min(0, page.At(0, 664).y);
    const int hovered = DrawHelpBar(dl, bar_min, io.DisplaySize, help, input_.Keyboard(), false, skin_);
    if (found) {
        const char* hint = input_.Keyboard() ? "F5 plays from any screen" : "Start plays from any screen";
        const TextStyle style = kCaps(19);
        const float w = TextWidth(style, hint);
        PaintText(dl, ImVec2(io.DisplaySize.x - Px(28) - w, (bar_min.y + io.DisplaySize.y) * 0.5f - Px(19) * 0.5f),
                  style, IM_COL32(195, 199, 210, 255), hint);
    }
    if (box_) {
        const int option = DrawBox(dl, ImVec2(0, 0), io.DisplaySize, *box_, skin_);
        if (option >= 0 && input_.MouseMoved()) box_->selected = option;
        if (option >= 0 && input_.Clicked() && ImGui::GetFrameCount() != box_frame_) {
            box_->selected = option;
            Handle(Action::kSelect);
        }
    } else if (hovered >= 0 && input_.Clicked()) {
        Handle(hovered == 0 ? Action::kSelect : Action::kBack);
    }
    EndScreen();
}

void HubScreen::DrawCard(ImDrawList* dl, const Page& page, bool found) {
    DrawPanel(dl, page.At(800, 150), page.At(1220, 570), skin_);
    const float x = 826, width = 368;
    float y = 176;
    const TextStyle heading = kCaps(text::kBoxTitle);
    const TextStyle key = kCaps(21);
    const auto at = [&](float px, float py) { return page.At(px, py); };
    // a line: a caps name on the left, its value on the right
    const auto line = [&](std::string_view name, std::string_view value, ImU32 value_color, float height = 36) {
        PaintText(dl, at(x, y + (height - 21) * 0.5f), key, color::kText, name);
        const float vw = TextWidth(key, value);
        PaintText(dl, ImVec2(at(x + width, 0).x - vw, at(0, y + (height - 21) * 0.5f).y), key, value_color, value);
        dl->AddLine(at(x, y + height), at(x + width, y + height), IM_COL32(255, 255, 255, 18));
        y += height;
    };
    const std::string root = rex::path_to_utf8(facts_root_.empty() ? std::filesystem::path("(not set)")
                                                                     : facts_root_);
    if (!found) {
        PaintText(dl, at(x, y), heading, color::kText, "Rock Band 3 not found", Px(width));
        y += 36;
        DrawRule(dl, at(x, y + 2), at(x + width, y + 6), skin_);
        y += 18;
        for (const std::string& l :
             WrapText(kPlain(text::kBody),
                      "band3 plays your own copy of Rock Band 3, copied from your Xbox 360 disc to this "
                      "PC. Find Rock Band 3 looks for it, then lets you browse.",
                      Px(width))) {
            PaintText(dl, at(x, y), kPlain(text::kBody), IM_COL32(217, 220, 229, 255), l);
            y += text::kBodyLine;
        }
        y += 12;
        PaintText(dl, at(x, y + 7), key, color::kText, "Folder now");
        const char* none = "No game here";
        const float vw = TextWidth(key, none);
        PaintText(dl, ImVec2(at(x + width, 0).x - vw, at(0, y + 7).y), key, color::kBad, none);
        dl->AddCircleFilled(ImVec2(at(x + width, 0).x - vw - Px(12), at(0, y + 18).y), Px(5), color::kBad);
        y += 40;
        for (const std::string& l : WrapText(kPlain(18), root, Px(width))) {
            PaintText(dl, at(x, y), kPlain(18), kCardMuted, l);
            y += 22;
        }
        return;
    }

    PaintText(dl, at(x, y), heading, color::kText, "Ready to play");
    y += 36;
    DrawRule(dl, at(x, y + 2), at(x + width, y + 6), skin_);
    y += 18;
    // Rock Band 3, found, and where
    PaintText(dl, at(x, y + 7), key, color::kText, "Rock Band 3");
    {
        const char* found_text = "Found";
        const float vw = TextWidth(key, found_text);
        PaintText(dl, ImVec2(at(x + width, 0).x - vw, at(0, y + 7).y), key, color::kText, found_text);
        dl->AddCircleFilled(ImVec2(at(x + width, 0).x - vw - Px(12), at(0, y + 18).y), Px(5), color::kGood);
        y += 34;
        PaintText(dl, at(x, y), kPlain(18), kCardMuted, root, Px(width));
        y += 26;
        dl->AddLine(at(x, y), at(x + width, y), IM_COL32(255, 255, 255, 18));
    }

    std::optional<std::string> deluxe;
    int songs = 0;
    std::optional<Played> last;
    bool done = false;
    if (facts_) {
        std::lock_guard lock(facts_->mutex);
        done = facts_->done;
        deluxe = facts_->deluxe;
        songs = facts_->songs;
        last = facts_->last;
    }
    if (done) {
        line("Deluxe", deluxe ? *deluxe : "Not installed", deluxe ? color::kText : kCardMuted);
        // the songs the game listed when it last ran: none before its first
        if (songs > 0) line("Songs", Thousands(songs), color::kText);
    }

    // the controllers plugged in, as RB3's icons
    PaintText(dl, at(x, y + 12), key, color::kText, "Controllers");
    std::vector<IconCell> icons;
    if (host_.controllers) {
        for (const HubController& c : host_.controllers()) {
            if (c.midi_drums) icons.push_back({2, 0});
            else if (c.midi_keys) icons.push_back({3, 0});
            else if (c.sub_type) {
                if (const auto icon = IconFor(*c.sub_type)) icons.push_back(*icon);
                else icons.push_back({-1, -1});
            }
        }
    }
    float ix = x + width - 30;
    if (icons.empty()) {
        const char* none = "None yet";
        const float vw = TextWidth(key, none);
        PaintText(dl, ImVec2(at(x + width, 0).x - vw, at(0, y + 12).y), key, kCardMuted, none);
    }
    for (auto it = icons.rbegin(); it != icons.rend() && ix > x + 170; ++it) {
        if (it->column < 0) {
            // a gamepad: no icon of its own in RB3's sheet
            const ImVec2 c = at(ix + 15, y + 22);
            dl->AddCircle(c, Px(13), color::kSoft, 0, Px(2));
            PaintText(dl, ImVec2(c.x - TextWidth(kCaps(12), "Pad") * 0.5f, c.y - Px(6)), kCaps(12), color::kSoft,
                      "Pad");
        } else {
            DrawIcon(dl, at(ix, y + 7), at(ix + 30, y + 37), skin_, it->column, it->row);
        }
        ix -= 36;
    }
    y += 44;
    dl->AddLine(at(x, y), at(x + width, y), IM_COL32(255, 255, 255, 18));

    if (last) {
        y += 12;
        PaintText(dl, at(x, y), kCaps(19), kCardMuted, "Last played");
        y += 26;
        const float tw = PaintText(dl, at(x, y), kPlain(22), color::kText, last->title, Px(width * 0.7f));
        PaintText(dl, ImVec2(at(x, 0).x + tw + Px(10), at(0, y + 1).y), kPlain(20), IM_COL32(191, 195, 207, 255),
                  last->artist, Px(width) - tw - Px(10));
    }
}

}
