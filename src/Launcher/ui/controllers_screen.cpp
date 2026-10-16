#include "controllers_screen.h"
#include <algorithm>
#include "hub_model.h"
#include "src/Launcher/device_view_model.h"
#include "src/Launcher/launcher_style.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace {

constexpr float kRowsLeft = 40, kRowsRight = 560, kTop = 140;
constexpr float kDeviceHeight = 48, kRowHeight = 40, kBand = 34;
constexpr float kPanelLeft = 576, kPanelRight = 1256;

// the settings rows after the devices
constexpr int kSettingsRows = 2;

std::optional<IconCell> EntryIcon(const DevicePanel::Entry& e) {
    if (e.kind == input::DeviceKind::kMidiDrums) return IconCell{2, 0};
    if (e.kind == input::DeviceKind::kMidiKeys) return IconCell{3, 0};
    if (e.sub_type) return IconFor(*e.sub_type);
    return std::nullopt;
}

}

ControllersScreen::ControllersScreen(Skin& skin, DevicePanel& devices, ControllersHost host)
    : skin_(skin), devices_(devices), host_(std::move(host)) {}

void ControllersScreen::Show() {
    input_.Show();
    selected_ = 0;
}

void ControllersScreen::Handle(Action action) {
    const int count = int(entries_.size()) + kSettingsRows;
    // while one's tested, everything but stopping it waits
    if (devices_.Testing()) {
        if (action == Action::kBack) devices_.EndTest();
        return;
    }
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        selected_ = MoveSelection(selected_, action == Action::kUp ? -1 : 1, count, false);
        break;
    case Action::kSelect: Activate(selected_); break;
    case Action::kBack:
        if (host_.back) host_.back();
        break;
    case Action::kPlay:
        if (host_.play) host_.play();
        break;
    default: break;
    }
}

void ControllersScreen::Activate(int index) {
    if (index < int(entries_.size())) {
        const DevicePanel::Entry& e = entries_[size_t(index)];
        devices_.Select(e.id);
        if (e.testable) devices_.BeginTest(e.id);
        return;
    }
    if (!host_.settings) return;
    host_.settings(index == int(entries_.size()) ? Category::kControllers : Category::kMidi);
}

void ControllersScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads) {
    entries_ = devices_.Entries();
    // SDL's copies of dongle instruments are their dongles' rows, and the
    // SDK's stand-in is no controller at all
    std::erase_if(entries_, [](const DevicePanel::Entry& e) { return e.copy || IsStandIn(e.kind, e.name); });
    const int count = int(entries_.size()) + kSettingsRows;
    selected_ = std::clamp(selected_, 0, count - 1);
    for (const Action a : input_.Read(pads)) Handle(a);
    if (selected_ < int(entries_.size()) && !devices_.Testing()) devices_.Select(entries_[size_t(selected_)].id);
    const std::optional<uint64_t> testing = devices_.Testing();

    const Page page = PageFor(io);
    ImDrawList* dl = BeginScreen("band3##controllers", io, skin_);
    DrawTitle(dl, page.At(70, 14), "Test Controllers");
    DrawStrip(dl, page.At(0, 96), page.At(kRowsRight, 132), skin_);
    std::string strip = std::to_string(entries_.size()) + (entries_.size() == 1 ? " device found" : " devices found");
    const DevicePanel::Entry* tested = nullptr;
    for (const auto& e : entries_) {
        if (testing == e.id) tested = &e;
    }
    if (tested) strip = "Testing: " + tested->name;
    PaintText(dl, page.At(40, 100), kCaps(text::kStrip), color::kStripText, strip,
              Px(kRowsRight - 50));

    // the devices, then their settings
    float y = kTop;
    DrawBand(dl, page.At(kRowsLeft, y), page.At(kRowsRight, y + kBand), "Devices", BandKind::kSection, skin_);
    y += kBand;
    if (devices_.Listed() && entries_.empty()) {
        PaintText(dl, page.At(kRowsLeft + 20, y + 10), kPlain(text::kBody), color::kMuted,
                  "No controllers found. Connect one, and it shows up here.");
        y += kRowHeight;
    }
    for (int i = 0; i < int(entries_.size()); i++) {
        const DevicePanel::Entry& e = entries_[size_t(i)];
        const ImVec2 min = page.At(kRowsLeft, y), max = page.At(kRowsRight, y + kDeviceHeight);
        const bool selected = i == selected_;
        const bool dim = testing && testing != e.id;
        const std::string player = PlayerLabel(e.player);
        RowLook look;
        look.label = "";
        look.value = player;
        look.selected = selected;
        look.dim = dim;
        look.value_caps = true;
        DrawRow(dl, min, max, look, skin_);
        const ImU32 text_color = selected ? color::kOnYellow : dim ? IM_COL32(244, 244, 244, 107) : color::kText;
        float x = kRowsLeft + 16;
        if (const auto icon = EntryIcon(e)) {
            DrawIcon(dl, page.At(x, y + 9), page.At(x + 30, y + 39), skin_, icon->column, icon->row);
        }
        x += 42;
        const float value_room = TextWidth(kCaps(text::kValue), look.value) / page.scale + 40;
        PaintText(dl, page.At(x, y + 3), kPlain(text::kRow), text_color, e.name,
                  Px(kRowsRight - kRowsLeft - 58 - value_room));
        PaintText(dl, page.At(x, y + 26), kCaps(18), selected ? color::kOnYellow : color::kMuted, e.kind_label,
                  Px(kRowsRight - kRowsLeft - 58 - value_room));
        if (!testing && ImGui::IsMouseHoveringRect(min, max)) {
            if (input_.MouseMoved()) selected_ = i;
            if (input_.Clicked()) {
                selected_ = i;
                Activate(i);
            }
        }
        y += kDeviceHeight + 1;
    }
    y += 14;
    DrawBand(dl, page.At(kRowsLeft, y), page.At(kRowsRight, y + kBand), "Their settings", BandKind::kSection, skin_);
    y += kBand;
    const char* settings[kSettingsRows] = {"Controllers", "MIDI"};
    for (int k = 0; k < kSettingsRows; k++) {
        const int index = int(entries_.size()) + k;
        const ImVec2 min = page.At(kRowsLeft, y), max = page.At(kRowsRight, y + kRowHeight);
        RowLook look;
        look.label = settings[k];
        look.value = "Open";
        look.value_caps = true;
        look.action = true;
        look.selected = index == selected_;
        look.dim = bool(testing);
        DrawRow(dl, min, max, look, skin_);
        if (!testing && ImGui::IsMouseHoveringRect(min, max)) {
            if (input_.MouseMoved()) selected_ = index;
            if (input_.Clicked()) Activate(index);
        }
        y += kRowHeight + 1;
    }

    // the panel: the selected device, live
    DrawPanel(dl, page.At(kPanelLeft, 96), page.At(kPanelRight, 650), skin_);
    const float px = kPanelLeft + 26, pw = kPanelRight - kPanelLeft - 52;
    float py = 120;
    if (selected_ >= int(entries_.size())) {
        const bool midi = selected_ > int(entries_.size());
        PaintText(dl, page.At(px, py), kCaps(text::kPanelHeading), color::kText, midi ? "MIDI" : "Controllers");
        py += 36;
        DrawRule(dl, page.At(px, py), page.At(px + pw, py + 4), skin_);
        py += 16;
        for (const std::string& line :
             WrapText(kPlain(text::kBody),
                      midi ? "Opens Settings at MIDI: a MIDI drum kit or keyboard, its port and pads."
                           : "Opens Settings at Controllers: what gamepads and guitars play as, the "
                             "sticks, and the delay per controller type.",
                      Px(pw))) {
            PaintText(dl, page.At(px, py), kPlain(text::kBody), color::kBody, line);
            py += text::kBodyLine;
        }
    } else if (!entries_.empty()) {
        const DevicePanel::Entry& e = entries_[size_t(selected_)];
        PaintText(dl, page.At(px, py), kCaps(26), color::kText, e.name, Px(pw));
        py += 34;
        if (testing == e.id) {
            PaintText(dl, page.At(px, py), kCaps(20), color::kInfo, "Testing - play it");
        } else {
            PaintText(dl, page.At(px, py), kCaps(20), color::kMuted, e.kind_label + " - " + PlayerLabel(e.player),
                      Px(pw));
        }
        py += 26;
        DrawRule(dl, page.At(px, py), page.At(px + pw, py + 4), skin_);
        py += 14;
        const char* what = testing == e.id
                               ? (input_.Keyboard() ? "While it's tested its buttons only light the drawing. Esc "
                                                      "stops the test, as does its own Back button, beside Start."
                                                    : "While it's tested its buttons only light the drawing. Its own "
                                                      "Back button, beside Start, stops the test, as does B on "
                                                      "another controller. (On a guitar, B is the red fret.)")
                           : e.testable ? "Select it to test it on its own: its frets, strum and whammy light up here, and its "
                                          "buttons stop moving the menu until you press its own Back button, beside "
                                          "Start."
                                        : "It moves the menus itself, so it isn't tested on its own: what it "
                                          "presses lights up here as you use it.";
        for (const std::string& line : WrapText(kPlain(text::kBody), what, Px(pw))) {
            PaintText(dl, page.At(px, py), kPlain(text::kBody), color::kBody, line);
            py += text::kBodyLine;
        }
        py += 12;
        // the live view, laid out with ImGui in the theme's type
        ImGui::SetCursorScreenPos(page.At(px, py));
        PushThemeFont(kPlain(20));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.95f, 0.95f, 0.95f, 1));
        if (ImGui::BeginChild("##live", ImVec2(Px(pw), page.At(0, 628).y - page.At(0, py).y),
                              ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoNav)) {
            devices_.DrawLive();
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::PopFont();
    }

    std::vector<HelpItem> help;
    if (testing) {
        if (input_.Keyboard()) {
            help = {{HelpButton::kB, "Stop test"}};
        } else {
            // its own Back button, not B (on a guitar, the red fret)
            help = {{HelpButton::kBack, "On it: stop test"}, {HelpButton::kB, "On another: stop test"}};
        }
    } else {
        const bool device = selected_ < int(entries_.size());
        if (!device) help.push_back({HelpButton::kA, "Open"});
        else if (entries_[size_t(selected_)].testable) help.push_back({HelpButton::kA, "Test"});
        help.push_back({HelpButton::kB, host_.back_label ? host_.back_label() : std::string("Back to menu")});
    }
    const int hovered = DrawHelpBar(dl, ImVec2(0, page.At(0, 664).y), io.DisplaySize, help, input_.Keyboard(),
                                    !testing, skin_);
    if (hovered >= 0 && input_.Clicked()) {
        if (hovered == kHelpPlay) Handle(Action::kPlay);
        else Handle(help[size_t(hovered)].button == HelpButton::kA ? Action::kSelect : Action::kBack);
    }
    EndScreen();
}

}
