#include "widget_gallery.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "keyboard_actions.h"
#include "src/Launcher/launcher_style.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace {

constexpr const char* kMenu[] = {"Play", "Settings", "Music Videos", "Test Controllers", "Quit"};

struct DemoRow {
    const char* label;
    const char* value;
    bool value_caps = false;
    bool on = false;
    bool error = false;
    bool locked = false;
    bool dim = false;
    bool list = false;
};
constexpr DemoRow kRows[] = {
    {"Renderer", "Native", false, false, false, false, false, true},
    {"Anti-aliasing", "High (8x)", false, false, false, false, false, true},
    {"Hair shader", "On", true, true},
    {"Web page port", "21070", false, false, true, false, false, true},
    {"Game data folder", "D:\\Games\\Rock Band 3", false, false, false, true},
    {"Compress character textures", "Off", true, false, false, false, true},
    {"A label long enough that it has to be cut short", "A value that's long too", false, false,
     false, false, false, true},
};
constexpr int kRowCount = int(std::size(kRows));

const char* ActionName(Action a) {
    switch (a) {
    case Action::kUp: return "Up";
    case Action::kDown: return "Down";
    case Action::kLeft: return "Left";
    case Action::kRight: return "Right";
    case Action::kSelect: return "Select";
    case Action::kBack: return "Back";
    case Action::kSecondary: return "Secondary";
    case Action::kReset: return "Reset";
    case Action::kPreviousCategory: return "Previous category";
    case Action::kNextCategory: return "Next category";
    case Action::kPlay: return "Play";
    }
    return "";
}

// every controller's buttons together, and the stick pushed furthest
NavPad Combined(std::span<const NavPad> pads) {
    NavPad out;
    for (const NavPad& p : pads) {
        out.buttons |= p.buttons;
        if (std::abs(p.left_x) > std::abs(out.left_x)) out.left_x = p.left_x;
        if (std::abs(p.left_y) > std::abs(out.left_y)) out.left_y = p.left_y;
    }
    return out;
}

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}

WidgetGallery::WidgetGallery(Skin& skin) : skin_(skin), changed_(kRowCount, false) {
    changed_[1] = true;
}

void WidgetGallery::Handle(Action action) {
    last_action_ = ActionName(action);
    switch (action) {
    case Action::kUp:
    case Action::kDown: {
        const int d = action == Action::kUp ? -1 : 1;
        if (column_ == 0) menu_ = MoveSelection(menu_, d, int(std::size(kMenu)), true);
        else row_ = MoveSelection(row_, d, kRowCount, false);
        break;
    }
    case Action::kLeft:
        column_ = 0;
        break;
    case Action::kRight:
        column_ = 1;
        break;
    case Action::kSelect:
        if (column_ == 1) changed_[size_t(row_)] = !changed_[size_t(row_)];
        else column_ = 1;
        break;
    case Action::kBack:
        column_ = 0;
        break;
    case Action::kReset:
        if (column_ == 1) changed_[size_t(row_)] = false;
        break;
    default:
        break;
    }
}

void WidgetGallery::Draw(ImGuiIO& io, std::span<const NavPad> pads) {
    // input: the controllers, then the keyboard; whichever was used last picks the hints
    const std::vector<Action> from_pads = pad_.Step(Combined(pads), Now());
    const std::vector<Action> from_keys = KeyboardActions();
    if (!from_pads.empty()) keyboard_ = false;
    if (!from_keys.empty()) keyboard_ = true;
    for (Action a : from_pads) Handle(a);
    for (Action a : from_keys) Handle(a);
    if (ImGui::IsKeyPressed(ImGuiKey_F, false)) shown_ = Shown((int(shown_) + 1) % 3);
    if (ImGui::IsKeyPressed(ImGuiKey_K, false)) keyboard_ = !keyboard_;
    const Skin& skin = shown_ == Shown::kPlain ? plain_skin_ : skin_;
    // the gallery's own pick, over launcher_style's
    SetLook(shown_ == Shown::kSimple ? Look::kSimple : Look::kRb3);

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    if (!ImGui::Begin("band3##widget_gallery", nullptr, flags)) {
        ImGui::End();
        return;
    }
    ImDrawList* dl = ImGui::GetWindowDrawList();
    DrawBackground(dl, ImVec2(0, 0), io.DisplaySize, skin);

    // the 1280x720 layout, scaled and centred in the window
    const float s = Scale();
    const ImVec2 origin((io.DisplaySize.x - 1280 * s) * 0.5f, (io.DisplaySize.y - 720 * s) * 0.5f);
    const auto at = [&](float x, float y) { return ImVec2(origin.x + x * s, origin.y + y * s); };

    DrawTitle(dl, at(70, 14), "Gallery");
    DrawStrip(dl, at(0, 96), at(890, 132), skin);
    size_t parts = 0;
    for (size_t i = 0; i < kSkinPartCount; i++) parts += skin.Has(static_cast<SkinPart>(i));
    char strip[96];
    std::snprintf(strip, sizeof(strip), "%s: %zu of %zu textures", shown_ == Shown::kSimple  ? "Simple look"
                  : shown_ == Shown::kPlain ? "Plain look"
                                            : "RB3 look",
                  parts, kSkinPartCount);
    PaintText(dl, at(40, 100), kCaps(text::kStrip), color::kStripText, strip);
    if (!last_action_.empty()) {
        const float w = TextWidth(kCaps(text::kHelp), last_action_);
        PaintText(dl, ImVec2(at(880, 101).x - w, at(880, 101).y), kCaps(text::kHelp), color::kGood,
                 last_action_);
    }

    // the menu: hovered items take the selection
    float y = 146;
    for (int i = 0; i < int(std::size(kMenu)); i++) {
        const ImVec2 pos = at(50, y);
        const ItemLook look = i == menu_ ? (column_ == 0 ? ItemLook::kSelected : ItemLook::kMarked)
                                         : (column_ == 0 ? ItemLook::kNormal : ItemLook::kFaint);
        const ImVec2 end = DrawMenuItem(dl, pos, kMenu[i], text::kCategory, look, skin, 200 * s);
        if (ImGui::IsMouseHoveringRect(pos, end) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            menu_ = i;
            column_ = 0;
        }
        y += 38;
    }

    // the rows, under a section's band, then an action band
    DrawBand(dl, at(300, 140), at(890, 174), "Rows", BandKind::kSection, skin, false, "7 settings");
    y = 174;
    for (int i = 0; i < kRowCount; i++) {
        const DemoRow& d = kRows[i];
        RowLook look;
        look.label = d.label;
        look.value = d.value;
        look.selected = column_ == 1 && i == row_;
        look.arrows = look.selected && d.list;
        look.value_caps = d.value_caps;
        look.on = d.on;
        look.error = d.error;
        look.locked = d.locked;
        look.dim = d.dim;
        look.changed = changed_[size_t(i)];
        const ImVec2 min = at(300, y), max = at(890, y + 40);
        DrawRow(dl, min, max, look, skin);
        if (ImGui::IsMouseHoveringRect(min, max) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
            row_ = i;
            column_ = 1;
        }
        if (ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Handle(Action::kSelect);
        }
        y += 41;
    }
    DrawBand(dl, at(300, y + 8), at(890, y + 42), "Find videos for every song", BandKind::kAction, skin);
    DrawBand(dl, at(300, y + 44), at(890, y + 78), "An action band, selected", BandKind::kAction, skin, true);

    // the detail panel
    DrawPanel(dl, at(906, 30), at(1256, 650), skin);
    float py = 56;
    PaintText(dl, at(932, py), kCaps(text::kPanelHeading), color::kText, kRows[row_].label, 290 * s);
    py += 38;
    DrawRule(dl, at(932, py), at(1230, py + 8), skin);
    py += 16;
    const char* body =
        "Smooths jagged edges. Each step asks more of the graphics card. Text wraps at the panel's "
        "width, in Montserrat SemiBold at 20 px a line.";
    for (const std::string& line : WrapText(kPlain(text::kBody), body, 296 * s)) {
        PaintText(dl, at(932, py), kPlain(text::kBody), color::kBody, line);
        py += text::kBodyLine;
    }
    py += 10;
    const std::pair<const char*, ChipKind> chips[] = {
        {"Changed: Y resets it", ChipKind::kChanged}, {"Applies when band3 restarts", ChipKind::kInfo},
        {"Rock Band 3 found here", ChipKind::kGood},  {"Recalibrate after this", ChipKind::kWarn},
        {"Port 21070 is in use", ChipKind::kBad},
    };
    for (const auto& [label, kind] : chips) {
        py += DrawChip(dl, at(932, py), label, kind, 296 * s).y / s + 8;
    }
    py += 6;
    float kx = 932;
    for (const char* key : {"Enter", "Esc", "LB", "RB", "Start"}) {
        kx += DrawKeyCap(dl, at(kx, py), key, 22 * s) / s + 8;
    }
    py += 34;
    for (int i = 0; i < 4; i++) DrawIcon(dl, at(932 + i * 40.0f, py), at(962 + i * 40.0f, py + 30), skin, i, 0);
    DrawIcon(dl, at(1092, py), at(1122, py + 30), skin, 0, 2);
    py += 44;
    PaintText(dl, at(932, py), kPlain(text::kBody), color::kMuted, "F: next look   K: key hints");

    const HelpItem help[] = {
        {HelpButton::kA, "Select"}, {HelpButton::kB, "Back"}, {HelpButton::kX, "Search"},
        {HelpButton::kY, "Reset"},
    };
    const int hovered = DrawHelpBar(dl, ImVec2(0, at(0, 664).y), ImVec2(io.DisplaySize.x, at(0, 720).y),
                                    help, keyboard_, true, skin);
    if (hovered >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        last_action_ = hovered == kHelpPlay ? std::string("Play (clicked)")
                                            : help[hovered].label + " (clicked)";
    }
    ImGui::End();
}

}
