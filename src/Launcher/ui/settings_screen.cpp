#include "settings_screen.h"
#include <SDL3/SDL_misc.h>
#include <imgui_internal.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include "keyboard_actions.h"
#include "keyboard_panel.h"
#include "src/Audio/usb_mic.h"
#include "src/Input/input_system.h"
#include "src/Launcher/device_names.h"
#include "src/Launcher/launcher_platform.h"
#include "src/Launcher/launcher_style.h"
#include "src/Lights/lights.h"
#include "src/paths.h"
#include "widgets.h"

namespace band3::launcher::ui {

namespace {

// the layout, at 1280x720
constexpr float kRowsLeft = 300, kRowsRight = 890, kRowsTop = 140, kRowsBottom = 652;
constexpr float kBandHeight = 34, kBandGap = 6, kRowHeight = 40, kRowPitch = 41;
constexpr float kSearchHeight = 46;
constexpr float kPanelLeft = 906, kPanelTop = 30, kPanelRight = 1256, kPanelBottom = 650;
constexpr float kBodyLeft = 932, kBodyWidth = 298, kBodyTop = 56, kBodyBottom = 628;
constexpr float kCategoriesLeft = 40, kCategoriesTop = 146, kCategoryPitch = 38;
constexpr float kDropLeft = 560, kDropWidth = 340, kDropItem = 36;
constexpr int kDropVisible = 11;
constexpr float kHelpTop = 664;

// a drop list's entry that types a value in instead of picking one
constexpr std::string_view kTypeOne = "\x01" "type";
// the folder list's entries: add one, or remove the one at an index
constexpr std::string_view kAddFolder = "\x01" "add";
constexpr std::string_view kRemoveFolder = "\x01" "remove:";

constexpr ImU32 kBodyText = IM_COL32(217, 220, 229, 255);
constexpr ImU32 kStripText = IM_COL32(232, 232, 236, 255);

double Now() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
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

std::string Number(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
}

bool MouseMoved(const ImGuiIO& io) { return io.MouseDelta.x != 0 || io.MouseDelta.y != 0; }

bool IsShowLauncher(const Row& row) {
    return row.kind == RowKind::kSetting && row.setting && row.setting->cvar == "show_launcher";
}

// a generated row (Advanced's): labelled with its name, described by the registry
bool Generated(const Row& row) {
    return row.kind == RowKind::kSetting && row.setting && row.setting->about.empty() &&
           row.setting->label == row.setting->cvar;
}

// the numbers left and right step, which A types instead
bool Slider(const Row& row) {
    if (row.kind != RowKind::kSetting || !row.setting) return false;
    switch (row.setting->widget) {
    case Widget::kFloatSlider:
    case Widget::kPercentSlider:
    case Widget::kIntSlider:
    case Widget::kMidiNote: return true;
    default: return false;
    }
}

// the Lights' test rows' choices
constexpr const char* kFogNames[] = {"Off", "On"};
constexpr const char* kStrobeNames[] = {"Off", "Slow", "Medium", "Fast", "Fastest"};
constexpr std::pair<const char*, uint8_t> kColours[] = {
    {"Red", lights::kRed}, {"Green", lights::kGreen}, {"Blue", lights::kBlue}, {"Yellow", lights::kYellow}};
constexpr std::pair<const char*, uint8_t> kPatterns[] = {
    {"All", lights::kPatternAll},     {"None", lights::kPatternNone},
    {"Odds", lights::kPatternOdds},   {"Evens", lights::kPatternEvens},
    {"Left", lights::kPatternLeft},   {"Right", lights::kPatternRight}};

int Wrap(int value, int delta, int count) { return ((value + delta) % count + count) % count; }

void OpenFolder(const std::filesystem::path& folder) {
    SDL_OpenURL(("file:///" + rex::path_to_utf8(folder.generic_wstring())).c_str());
}

}

SettingsScreen::SettingsScreen(SettingsModel& model, Skin& skin, SettingsScreenHost host)
    : model_(model),
      skin_(skin),
      host_(std::move(host)),
      options_{.where = host_.where, .sdk_settings = bool(host_.sdk_settings)},
      devices_(host_.native_window) {}

SettingsScreen::~SettingsScreen() = default;

void SettingsScreen::Open(Category category) {
    CloseSearch();
    SetCategory(category);
    focus_ = Focus::kCategories;
}

void SettingsScreen::ShowBox(Box box) {
    drop_.reset();
    box_ = std::move(box);
    opened_frame_ = ImGui::GetFrameCount();
}

ImVec2 SettingsScreen::At(float x, float y) const {
    return ImVec2(origin_.x + x * scale_, origin_.y + y * scale_);
}

const GameDataCheck& SettingsScreen::GameData(bool now) {
    using Clock = std::chrono::steady_clock;
    const std::filesystem::path root = host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (now || !check_done_ || root != checked_root_ || Clock::now() >= next_check_) {
        checked_root_ = root;
        check_ = CheckGameData(root);
        check_done_ = true;
        next_check_ = Clock::now() + std::chrono::seconds(1);
    }
    return check_;
}

// State

int SettingsScreen::RowCount() const {
    int count = 0;
    for (const Section& s : sections_) count += int(s.rows.size());
    return count;
}

const Row* SettingsScreen::SelectedRow() const {
    int i = 0;
    for (const Section& s : sections_) {
        for (const Row& r : s.rows) {
            if (i++ == row_) return &r;
        }
    }
    return nullptr;
}

bool SettingsScreen::Locked(const Row& row) const {
    if (!row.setting || row.kind == RowKind::kControl) return false;
    const Setting& s = *row.setting;
    return model_.IsLocked(s.cvar) || (!s.companion.empty() && model_.IsLocked(s.companion)) ||
           model_.ReadOnly(s.cvar);
}

std::string SettingsScreen::LockedWhy(const Row& row) const {
    if (!row.setting) return {};
    const Setting& s = *row.setting;
    if (s.command_line_only) return "Set on the command line only";
    if (model_.ReadOnly(s.cvar)) return "Change this from the launcher";
    const Lock lock = model_.IsLocked(s.cvar) ? model_.LockOf(s.cvar) : model_.LockOf(s.companion);
    return LockReason(lock);
}

std::string SettingsScreen::RowValue(const Row& row) const {
    if (typing_ && !typing_->search && typing_->row.setting == row.setting &&
        typing_->row.index == row.index && typing_->row.kind == row.kind) {
        if (!typing_->secret) return keyboard_.Text();
        std::string dots;
        for (size_t i = 0; i < keyboard_.Text().size(); i++) {
            if ((static_cast<unsigned char>(keyboard_.Text()[i]) & 0xC0) != 0x80) dots += "\xE2\x80\xA2";
        }
        return dots;
    }
    if (IsShowLauncher(row)) return model_.ShowAtStartup() ? "On" : "Off";
    if (row.kind == RowKind::kControl) {
        switch (row.control) {
        case Control::kSettingsFile: {
            const auto path = host_.config_path ? host_.config_path() : std::filesystem::path();
            return path.empty() ? "band3.toml" : rex::path_to_utf8(path.filename());
        }
        case Control::kLowestDelay: return "Apply";
        case Control::kTestControllers:
        case Control::kVideosList:
        case Control::kSdkSettings: return "Open";
        case Control::kLightsDevice: {
            if (!lights_target_) return "All kits";
            for (const lights::DeviceRow& d : lights::Devices()) {
                if (d.key == *lights_target_) return d.name;
            }
            return "All kits";
        }
        case Control::kLightsCheck:
        case Control::kChase: return "Play";
        case Control::kFog: return kFogNames[lights_fog_];
        case Control::kStrobe: return kStrobeNames[lights_strobe_];
        case Control::kLightsColour: return kColours[lights_colour_].first;
        case Control::kLeds: return std::to_string(lights_led_ + 1);
        case Control::kPattern: return kPatterns[lights_pattern_].first;
        case Control::kLightsOff: return "Send";
        }
    }
    return ValueText(model_, row, devices_.Lists());
}

void SettingsScreen::Note(std::string text, ImU32 color) {
    note_ = std::move(text);
    note_color_ = color;
    note_until_ = std::chrono::steady_clock::now() + std::chrono::seconds(4);
}

// Input

void SettingsScreen::Handle(Action action) {
    if (box_) {
        HandleBox(action);
        return;
    }
    if (typing_) {
        HandleTyping(keyboard_.Handle(action));
        return;
    }
    if (drop_) {
        HandleDrop(action);
        return;
    }
    switch (action) {
    case Action::kPreviousCategory:
    case Action::kNextCategory: {
        const auto all = Categories();
        const int at = int(std::ranges::find(all, category_) - all.begin());
        const int next = Wrap(at, action == Action::kNextCategory ? 1 : -1, int(all.size()));
        if (search_) CloseSearch();
        SetCategory(all[size_t(next)]);
        return;
    }
    case Action::kPlay: Leave(After::kPlay); return;
    case Action::kSecondary: OpenSearch(); return;
    default: break;
    }
    if (focus_ == Focus::kRows) {
        HandleRows(action);
    } else {
        HandleCategories(action);
    }
}

void SettingsScreen::HandleCategories(Action action) {
    const auto all = Categories();
    const int at = int(std::ranges::find(all, category_) - all.begin());
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        SetCategory(all[size_t(MoveSelection(at, action == Action::kUp ? -1 : 1, int(all.size()), true))]);
        break;
    case Action::kSelect:
        if (RowCount() > 0) {
            focus_ = Focus::kRows;
            row_ = 0;
            reveal_ = true;
        }
        break;
    case Action::kBack: Leave(After::kLeave); break;
    default: break;
    }
}

void SettingsScreen::HandleRows(Action action) {
    const Row* row = SelectedRow();
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        row_ = std::max(0, MoveSelection(row_, action == Action::kUp ? -1 : 1, RowCount(), false));
        reveal_ = true;
        break;
    case Action::kLeft:
    case Action::kRight:
        if (row) Step(*row, action == Action::kLeft ? -1 : 1);
        break;
    case Action::kSelect:
        if (row) Activate(*row);
        break;
    case Action::kReset:
        if (row) ResetRow(*row);
        break;
    case Action::kBack:
        if (search_) {
            CloseSearch();
        } else {
            focus_ = Focus::kCategories;
        }
        break;
    default: break;
    }
}

void SettingsScreen::HandleDrop(Action action) {
    DropList& drop = *drop_;
    const int count = int(drop.items.size());
    switch (action) {
    case Action::kUp:
    case Action::kDown:
        drop.selected = MoveSelection(drop.selected, action == Action::kUp ? -1 : 1, count, true);
        drop.scroll = std::clamp(drop.scroll, drop.selected - kDropVisible + 1, drop.selected);
        break;
    case Action::kSelect: PickDrop(drop.selected); break;
    case Action::kBack: drop_.reset(); break;
    default: break;
    }
}

void SettingsScreen::HandleBox(Action action) {
    if (const auto picked = StepBox(*box_, action)) {
        const auto chosen = std::move(box_->chosen);
        box_.reset();
        if (chosen) chosen(*picked);
    }
}

void SettingsScreen::HandleTyping(OnScreenKeyboard::Result result) {
    if (!typing_) return;
    if (typing_->search) {
        search_ = keyboard_.Text();
        if (result == OnScreenKeyboard::Result::kNone) {
            row_ = 0;
            scroll_ = 0;
            return;
        }
        typing_.reset();
        // nothing typed: back where the search started
        if (search_->empty()) {
            CloseSearch();
        } else {
            focus_ = Focus::kRows;
            row_ = 0;
            reveal_ = true;
        }
        return;
    }
    if (result == OnScreenKeyboard::Result::kNone) return;
    const Typing typing = *typing_;
    typing_.reset();
    if (result != OnScreenKeyboard::Result::kDone) return;
    std::string value = keyboard_.Text();
    // a percent typed as one (15, not 0.15)
    if (typing.row.setting && typing.row.setting->widget == Widget::kPercentSlider) {
        if (const auto v = AsFloat(value)) value = Number(*v / 100);
    }
    Apply(typing.row, value);
}

void SettingsScreen::TypeFromKeyboard(ImGuiIO& io) {
    bool edited = false;
    const OnScreenKeyboard::Result result = TypeIntoKeyboard(keyboard_, io, edited);
    if (edited) {
        keyboard_hints_ = true;
        HandleTyping(OnScreenKeyboard::Result::kNone);
    }
    if (result != OnScreenKeyboard::Result::kNone) HandleTyping(result);
}
// What the rows do

void SettingsScreen::Activate(const Row& row) {
    if (row.kind == RowKind::kControl) {
        RunControl(row, 0);
        return;
    }
    if (Locked(row)) {
        Note(LockedWhy(row), color::kWarn);
        return;
    }
    if (IsShowLauncher(row)) {
        model_.SetShowAtStartup(!model_.ShowAtStartup());
        return;
    }
    const Setting& s = *row.setting;
    if (s.cvar == "game_data_root" && host_.find_game) {
        host_.find_game();
        return;
    }
    if (row.kind == RowKind::kSetting && s.widget == Widget::kPath) {
        std::string from = model_.Value(s.cvar);
        from = from.empty() ? model_.EffectiveDefault(s.cvar)
                            : rex::path_to_utf8(paths::Resolve(from, model_.Env().anchor));
        picker_.Start(std::string(s.cvar), from);
        Note("Pick the folder in the window that opened", color::kInfo);
        return;
    }
    if (OpensList(row)) {
        OpenDrop(row, 0);
        return;
    }
    if (TypesText(row) || Slider(row)) {
        OpenTyping(row);
        return;
    }
    const std::vector<ChoiceItem> choices = Choices(model_, row, devices_.Lists());
    if (choices.empty()) return;
    // A goes round a short list
    const auto current = std::ranges::find_if(choices, &ChoiceItem::current);
    const int at = current == choices.end() ? -1 : int(current - choices.begin());
    Apply(row, choices[size_t(Wrap(at, 1, int(choices.size())))].value);
}

void SettingsScreen::Step(const Row& row, int delta) {
    if (row.kind == RowKind::kControl) {
        RunControl(row, delta);
        return;
    }
    if (Locked(row)) {
        Note(LockedWhy(row), color::kWarn);
        return;
    }
    if (IsShowLauncher(row)) {
        model_.SetShowAtStartup(delta > 0);
        return;
    }
    if (const auto value = Stepped(model_, row, delta, devices_.Lists())) Apply(row, *value);
}

void SettingsScreen::Apply(const Row& row, const std::string& value) {
    if (!row.setting) return;
    const std::string cvar(row.setting->cvar);
    if (ApplyValue(model_, row, value)) {
        problems_.erase(cvar);
        if (cvar == "frame_cap" || cvar == "native_present_pacing") undo_.clear();
    } else {
        problems_[cvar] = model_.Refusal(cvar, value);
    }
}

void SettingsScreen::ResetRow(const Row& row) {
    if (row.kind == RowKind::kControl) {
        if (row.control == Control::kLowestDelay && !undo_.empty()) {
            for (const auto& [cvar, value] : undo_) model_.Set(cvar, value);
            undo_.clear();
            Note("Lowest delay undone", color::kGood);
        }
        return;
    }
    if (Locked(row) || IsShowLauncher(row) || !RowChanged(model_, row)) return;
    if (ui::ResetRow(model_, row)) {
        if (row.setting) problems_.erase(std::string(row.setting->cvar));
        Note("Reset " + row.label + " to " + RowValue(row), color::kGood);
    }
}

void SettingsScreen::RunControl(const Row& row, int delta) {
    using namespace band3::lights;
    switch (row.control) {
    case Control::kSettingsFile:
        if (delta == 0 && host_.config_path) OpenFolder(host_.config_path().parent_path());
        break;
    case Control::kLowestDelay: {
        if (delta != 0) break;
        const double hz = DisplayRefreshRate(host_.native_window ? host_.native_window() : nullptr);
        undo_.clear();
        undo_.push_back({"frame_cap", model_.Value("frame_cap")});
        model_.Set("frame_cap", LowestLatencyCap(hz));
        const Setting* pacing = model_.Find("native_present_pacing");
        if (LowestLatencyPacingOff(hz) && pacing && model_.Available(*pacing)) {
            undo_.push_back({"native_present_pacing", model_.Value("native_present_pacing")});
            model_.Set("native_present_pacing", "false");
        }
        Note(keyboard_hints_ ? "Lowest delay set - R undoes it" : "Lowest delay set - Y undoes it",
             color::kGood);
        break;
    }
    case Control::kTestControllers:
        if (delta == 0 && host_.test_controllers) host_.test_controllers();
        break;
    case Control::kVideosList:
        if (delta == 0 && host_.videos_list) host_.videos_list();
        break;
    case Control::kSdkSettings:
        if (delta == 0 && host_.sdk_settings) host_.sdk_settings();
        break;
    case Control::kLightsDevice: {
        const std::vector<DeviceRow> devices = Devices();
        if (delta == 0) {
            OpenDrop(row, 0);
            break;
        }
        int at = -1;
        for (size_t i = 0; i < devices.size(); i++) {
            if (lights_target_ && devices[i].key == *lights_target_) at = int(i);
        }
        const int next = Wrap(at + 1, delta, int(devices.size()) + 1) - 1;
        lights_target_ = next < 0 ? std::nullopt : std::optional<std::string>(devices[size_t(next)].key);
        break;
    }
    case Control::kLightsCheck:
        if (delta == 0) PlayScene(ColourCheck(), lights_target_);
        break;
    case Control::kFog:
        lights_fog_ = delta == 0 ? 1 - lights_fog_ : std::clamp(lights_fog_ + delta, 0, 1);
        SendTest({0, lights_fog_ ? kFogOn : kFogOff}, lights_target_);
        break;
    case Control::kStrobe:
        if (delta != 0) lights_strobe_ = std::clamp(lights_strobe_ + delta, 0, int(std::size(kStrobeNames)) - 1);
        SendTest({0, uint8_t(lights_strobe_ ? kStrobeSlow + lights_strobe_ - 1 : kStrobeOff)}, lights_target_);
        break;
    case Control::kLightsColour:
        lights_colour_ = Wrap(lights_colour_, delta == 0 ? 1 : delta, int(std::size(kColours)));
        break;
    case Control::kLeds:
        if (delta != 0) lights_led_ = std::clamp(lights_led_ + delta, 0, 7);
        SendTest({uint8_t(1 << lights_led_), kColours[lights_colour_].second}, lights_target_);
        break;
    case Control::kPattern:
        if (delta != 0) lights_pattern_ = std::clamp(lights_pattern_ + delta, 0, int(std::size(kPatterns)) - 1);
        SendTest({kPatterns[lights_pattern_].second, kColours[lights_colour_].second}, lights_target_);
        break;
    case Control::kChase:
        if (delta == 0) PlayScene(Chase(), lights_target_);
        break;
    case Control::kLightsOff:
        if (delta == 0) SendTest(kAllOffCommand, lights_target_);
        break;
    }
}

void SettingsScreen::OpenDrop(const Row& row, float top) {
    DropList drop{.row = row, .top = top};
    if (row.kind == RowKind::kControl && row.control == Control::kLightsDevice) {
        drop.items.push_back({.label = "All kits", .value = "", .current = !lights_target_});
        for (const lights::DeviceRow& d : lights::Devices()) {
            drop.items.push_back({.label = d.kind + "  " + d.name, .value = d.key,
                                  .current = lights_target_ == d.key});
        }
    } else if (row.setting && row.setting->widget == Widget::kFolderList) {
        const auto folders = paths::SplitList(model_.Value(row.setting->cvar));
        for (size_t i = 0; i < folders.size(); i++) {
            drop.items.push_back({.label = folders[i],
                                  .value = std::string(kRemoveFolder) + std::to_string(i)});
        }
        drop.items.push_back({.label = "Add a folder...", .value = std::string(kAddFolder)});
    } else {
        drop.items = Choices(model_, row, devices_.Lists());
        // where the list has room for a value typed in
        const bool typed = row.kind == RowKind::kMicSlot ||
                           (row.setting && (row.setting->widget == Widget::kComboText ||
                                            row.setting->widget == Widget::kMidiPort ||
                                            row.setting->widget == Widget::kResolution));
        if (typed) drop.items.push_back({.label = "Type one...", .value = std::string(kTypeOne)});
    }
    if (drop.items.empty()) return;
    for (size_t i = 0; i < drop.items.size(); i++) {
        if (drop.items[i].current) drop.selected = int(i);
    }
    drop.scroll = std::max(0, drop.selected - kDropVisible + 1);
    drop_ = std::move(drop);
    opened_frame_ = ImGui::GetFrameCount();
}

void SettingsScreen::PickDrop(int index) {
    if (!drop_ || index < 0 || index >= int(drop_->items.size())) return;
    const DropList drop = std::move(*drop_);
    drop_.reset();
    const std::string& value = drop.items[size_t(index)].value;
    if (drop.row.kind == RowKind::kControl) {
        lights_target_ = value.empty() ? std::nullopt : std::optional<std::string>(value);
        return;
    }
    if (value == kTypeOne) {
        OpenTyping(drop.row);
        return;
    }
    const Setting& s = *drop.row.setting;
    if (value == kAddFolder) {
        picker_.Start(std::string(s.cvar), rex::path_to_utf8(model_.Env().anchor));
        Note("Pick the folder in the window that opened", color::kInfo);
        return;
    }
    if (value.starts_with(kRemoveFolder)) {
        auto folders = paths::SplitList(model_.Value(s.cvar));
        const size_t at = size_t(std::stoul(value.substr(kRemoveFolder.size())));
        if (at < folders.size()) {
            const std::string removed = folders[at];
            folders.erase(folders.begin() + std::ptrdiff_t(at));
            std::string list;
            for (const std::string& f : folders) list += (list.empty() ? "" : "|") + f;
            Apply(drop.row, list);
            Note("Removed " + removed, color::kGood);
        }
        return;
    }
    Apply(drop.row, value);
}

void SettingsScreen::OpenTyping(const Row& row) {
    if (!row.setting) return;
    const Setting& s = *row.setting;
    Typing typing{.row = row, .secret = s.widget == Widget::kPassword};
    std::string text;
    if (s.widget == Widget::kPercentSlider) {
        text = std::to_string(std::lround(AsFloat(model_.Value(s.cvar)).value_or(0) * 100));
    } else if (row.kind == RowKind::kMicSlot) {
        text = "";
    } else if (!typing.secret) {
        text = model_.Value(s.cvar);
    }
    keyboard_.Open(text, s.cvar == "username" ? 15 : 0);
    ShowSteamKeyboard();
    typing_ = typing;
    opened_frame_ = ImGui::GetFrameCount();
}

void SettingsScreen::OpenSearch() {
    drop_.reset();
    if (!search_) search_ = "";
    keyboard_.Open(*search_);
    ShowSteamKeyboard();
    typing_ = Typing{.search = true};
    opened_frame_ = ImGui::GetFrameCount();
    focus_ = Focus::kRows;
    row_ = 0;
    scroll_ = 0;
}

void SettingsScreen::CloseSearch() {
    if (typing_ && typing_->search) typing_.reset();
    if (!search_) return;
    search_.reset();
    focus_ = Focus::kCategories;
    row_ = 0;
    scroll_ = 0;
}

void SettingsScreen::SetCategory(Category category) {
    if (category != category_) {
        row_ = 0;
        scroll_ = 0;
    }
    category_ = category;
    drop_.reset();
}

void SettingsScreen::RequestLeave() { Leave(After::kLeave); }

void SettingsScreen::Leave(After after) {
    drop_.reset();
    typing_.reset();
    if (!model_.HasUnsavedChanges()) {
        Finish(after);
        return;
    }
    const SaveResult result = host_.save ? host_.save() : SaveResult{.ok = true};
    if (!result.ok) {
        // the system's messages end in a full stop of their own ("Access is denied.")
        std::string reason = result.error;
        while (!reason.empty() && (reason.back() == '.' || reason.back() == ' ')) reason.pop_back();
        ShowBox(Box{.title = "Couldn't save your settings",
                    .lines = {reason + ".", "Your changes still apply until band3 closes."},
                    .options = {"Try again", "Keep for this session", "Cancel"},
                    .cancel = 2,
                    .chosen = [this, after, reason](int choice) {
                        if (choice == 0) Leave(after);
                        if (choice == 1) {
                            kept_unsaved_ = reason;
                            Finish(after);
                        }
                    }});
        return;
    }
    kept_unsaved_.reset();
    // in game: what the game took as it started waits for the next start
    std::vector<std::string> waiting;
    if (host_.where == Where::kInGame) {
        for (const Setting& s : model_.Table()) {
            if (model_.Available(s) && s.widget != Widget::kNone && model_.WaitsForNextStart(s.cvar)) {
                waiting.push_back(model_.Label(s));
            }
        }
    }
    if (waiting.empty()) {
        Finish(after);
        return;
    }
    std::string list;
    for (size_t i = 0; i < waiting.size() && i < 6; i++) list += (i ? ", " : "") + waiting[i];
    if (waiting.size() > 6) list += " and " + std::to_string(waiting.size() - 6) + " more";
    Box box{.title = waiting.size() == 1 ? "1 change needs a restart"
                                         : std::to_string(waiting.size()) + " changes need a restart",
            .lines = {list + ".", "They're saved, and apply when band3 starts again."},
            .options = {"Later, at the next start"},
            .cancel = 0,
            .chosen = [this, after](int choice) {
                if (choice == 1 && host_.restart) {
                    host_.restart();
                } else {
                    Finish(after);
                }
            }};
    if (host_.restart) box.options.push_back("Restart band3 now");
    ShowBox(std::move(box));
}

void SettingsScreen::Finish(After after) {
    if (after == After::kPlay) {
        if (host_.play) host_.play();
    } else if (host_.left) {
        host_.left();
    }
}

void SettingsScreen::TakeFolderPick() {
    const auto answer = picker_.Take();
    if (!answer) return;
    if (!answer->error.empty()) {
        REXLOG_WARN("Launcher: the folder dialog failed: {}", answer->error);
        problems_[answer->target] = "The folder window didn't open: " + answer->error;
        return;
    }
    if (!answer->folder) return;
    const Setting* s = model_.Find(answer->target);
    if (!s) return;
    const Row row{.kind = RowKind::kSetting, .setting = s, .label = std::string(s->label)};
    if (s->widget == Widget::kFolderList) {
        std::string list = model_.Value(s->cvar);
        if (!paths::SplitList(list).empty()) list += '|';
        Apply(row, list + *answer->folder);
    } else {
        Apply(row, *answer->folder);
    }
}

// Drawing

void SettingsScreen::Draw(ImGuiIO& io, std::span<const NavPad> pads, bool over_game) {
    over_game_ = over_game;
    now_ = Now();
    scale_ = Scale();
    origin_ = ImVec2(std::round((io.DisplaySize.x - 1280 * scale_) * 0.5f),
                     std::round((io.DisplaySize.y - 720 * scale_) * 0.5f));
    TakeFolderPick();
    GameData(false);
    devices_.Refresh(model_, category_);

    // the rows showing, before the input moves over them
    sections_ = search_ ? SearchRows(model_, *search_, options_) : CategoryRows(model_, category_, options_);
    row_ = std::clamp(row_, 0, std::max(0, RowCount() - 1));

    // input: the controllers, then the keyboard; whichever was used last picks the hints
    const std::vector<Action> from_pads = pads_.Step(Combined(pads), now_);
    if (!from_pads.empty()) keyboard_hints_ = false;
    std::vector<Action> from_keys;
    if (ImGui::GetFrameCount() <= shown_frame_ + 1) {
        // the keys that opened it
    } else if (typing_) {
        TypeFromKeyboard(io);
        // the arrows still move over the keys
        for (const Action a : KeyboardActions()) {
            if (a == Action::kUp || a == Action::kDown || a == Action::kLeft || a == Action::kRight ||
                a == Action::kPlay) {
                from_keys.push_back(a);
            }
        }
    } else {
        from_keys = KeyboardActions();
    }
    if (!from_keys.empty()) keyboard_hints_ = true;
    for (const Action a : from_pads) Handle(a);
    for (const Action a : from_keys) Handle(a);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) Handle(Action::kBack);
    // what the input changed
    sections_ = search_ ? SearchRows(model_, *search_, options_) : CategoryRows(model_, category_, options_);
    row_ = std::clamp(row_, 0, std::max(0, RowCount() - 1));

    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoBackground;
    if (ImGui::Begin("band3##settings_screen", nullptr, flags)) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        if (over_game_) {
            DrawGameDim(dl, ImVec2(0, 0), io.DisplaySize, skin_);
        } else {
            DrawBackground(dl, ImVec2(0, 0), io.DisplaySize, skin_);
        }
        DrawTitle(dl, At(70, 14), "Settings");
        DrawStrip(dl);
        DrawCategories(dl);
        DrawRows(dl, io);
        DrawPanel(dl);
        DrawHelp(dl, io);
        if (drop_) DrawDrop(dl);
        // a click away from the keyboard is done with it: a search keeps
        // what's typed, a row's value is left as it was
        if (typing_ && !box_ && Clicked() &&
            !ImGui::IsMouseHoveringRect(At(kPanelLeft, kPanelTop), At(kPanelRight, kPanelBottom)) &&
            !ImGui::IsMouseHoveringRect(ImVec2(0, At(0, kHelpTop).y), io.DisplaySize)) {
            HandleTyping(typing_->search ? OnScreenKeyboard::Result::kDone
                                         : OnScreenKeyboard::Result::kCancelled);
        }
        if (box_) {
            const int hovered = ui::DrawBox(dl, ImVec2(0, 0), io.DisplaySize, *box_, skin_);
            if (hovered >= 0 && MouseMoved(io)) box_->selected = hovered;
            if (hovered >= 0 && Clicked()) {
                box_->selected = hovered;
                HandleBox(Action::kSelect);
            }
        }
    }
    ImGui::End();
    devices_.EndFrame();
}

void SettingsScreen::DrawStrip(ImDrawList* dl) {
    ui::DrawStrip(dl, At(0, 96), At(890, 132), skin_);
    const ImGuiIO& io = ImGui::GetIO();
    const bool free = !box_ && !typing_ && !drop_;
    float x = 40;
    const float cy = 114;
    if (search_) {
        x += PaintText(dl, At(x, cy - text::kStrip * 0.5f), kCaps(text::kStrip), kStripText, "Search") /
                 scale_ + 14;
        const int n = RowCount();
        PaintText(dl, At(x, cy - text::kHelp * 0.5f), kCaps(text::kHelp), color::kMuted,
                  std::to_string(n) + (n == 1 ? " result" : " results"));
    } else {
        const char* prev = keyboard_hints_ ? "Shift Tab" : "LB";
        const char* next = keyboard_hints_ ? "Tab" : "RB";
        const ImVec2 pmin = At(x, cy - 11);
        const float pw = DrawKeyCap(dl, pmin, prev, Px(22), true) / scale_;
        if (free && ImGui::IsMouseHoveringRect(pmin, At(x + pw, cy + 11)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Handle(Action::kPreviousCategory);
        }
        x += pw + 12;
        x += PaintText(dl, At(x, cy - text::kStrip * 0.5f), kCaps(text::kStrip), kStripText,
                       CategoryName(category_)) /
                 scale_ + 12;
        const ImVec2 nmin = At(x, cy - 11);
        const float nw = DrawKeyCap(dl, nmin, next, Px(22), true) / scale_;
        if (free && ImGui::IsMouseHoveringRect(nmin, At(x + nw, cy + 11)) &&
            ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            Handle(Action::kNextCategory);
        }
    }
    (void)io;

    // how the changes stand, or what was just done
    std::string status;
    ImU32 status_color = color::kGood;
    if (!note_.empty() && std::chrono::steady_clock::now() < note_until_) {
        status = note_;
        status_color = note_color_;
    } else if (kept_unsaved_) {
        status = "Not saved: " + *kept_unsaved_;
        status_color = color::kBad;
    } else if (model_.HasUnsavedChanges()) {
        status = "Saves as you leave";
        status_color = IM_COL32(233, 227, 160, 255);
    } else {
        status = "All changes saved";
        status_color = IM_COL32(191, 231, 191, 255);
    }
    const TextStyle style = kCaps(text::kHelp);
    const float room = Px(880 - 420);
    const float w = std::min(TextWidth(style, status), room);
    PaintText(dl, ImVec2(At(880, 0).x - w, At(0, cy - text::kHelp * 0.5f).y), style, status_color, status,
              room);
}

void SettingsScreen::DrawCategories(ImDrawList* dl) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool free = !box_ && !typing_ && !drop_;
    const auto all = Categories();
    for (size_t i = 0; i < all.size(); i++) {
        const Category c = all[i];
        const float y = kCategoriesTop + float(i) * kCategoryPitch;
        const ImVec2 min = At(kCategoriesLeft - 10, y + 1), max = At(kCategoriesLeft + 236, y + kCategoryPitch - 1);
        const bool current = !search_ && c == category_;
        ImU32 col = color::kText;
        if (current && focus_ == Focus::kCategories) {
            DrawHighlight(dl, min, max, skin_, Px(4));
            col = color::kOnYellow;
        } else if (current) {
            DrawHighlight(dl, min, max, skin_, Px(4), 0.26f);
        } else if (focus_ == Focus::kRows || search_) {
            col = IM_COL32(242, 242, 242, 140);
        }
        const int changed = model_.ChangedIn(c);
        float count_w = 0;
        if (changed > 0) count_w = DrawCount(dl, max.x - Px(8), (min.y + max.y) * 0.5f, changed) + Px(8);
        PaintText(dl, At(kCategoriesLeft + 2, y + (kCategoryPitch - text::kCategory) * 0.5f),
                  kCaps(text::kCategory), col, CategoryName(c), max.x - min.x - Px(14) - count_w);
        if (free && ImGui::IsMouseHoveringRect(min, max) && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            if (search_) CloseSearch();
            SetCategory(c);
            focus_ = Focus::kCategories;
        }
    }
    (void)io;
}

void SettingsScreen::DrawRows(ImDrawList* dl, ImGuiIO& io) {
    const bool free = !box_ && !typing_ && !drop_;
    // the layout: a band per section, its rows under it
    struct Placed {
        float y;
        const Section* section;
        const Row* row;
        int index;
    };
    std::vector<Placed> placed;
    float y = search_ ? kSearchHeight + 8 : 0;
    int index = 0;
    for (const Section& s : sections_) {
        if (!placed.empty() || y > 0) y += kBandGap;
        placed.push_back({y, &s, nullptr, -1});
        y += kBandHeight;
        if (s.note) {
            const auto lines = WrapText(kPlain(text::kBody), *s.note, Px(kRowsRight - kRowsLeft - 28));
            y += float(lines.size()) * text::kBodyLine + 8;
        }
        for (const Row& r : s.rows) {
            placed.push_back({y, &s, &r, index++});
            y += kRowPitch;
        }
    }
    const float content = y;
    const float area = kRowsBottom - kRowsTop;

    // the selected row in sight, its band too when it's the first
    if (reveal_) {
        for (size_t i = 0; i < placed.size(); i++) {
            if (placed[i].index != row_) continue;
            float top = placed[i].y;
            if (i > 0 && placed[i - 1].row == nullptr) top = placed[i - 1].y;
            if (row_ == 0) top = 0;
            if (top < scroll_) scroll_ = top;
            if (placed[i].y + kRowHeight > scroll_ + area) scroll_ = placed[i].y + kRowHeight - area;
        }
        reveal_ = false;
    }
    const ImVec2 area_min = At(kRowsLeft, kRowsTop), area_max = At(kRowsRight, kRowsBottom);
    if (free && ImGui::IsMouseHoveringRect(area_min, area_max) && io.MouseWheel != 0) {
        scroll_ -= io.MouseWheel * kRowPitch * 3;
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, content - area));

    dl->PushClipRect(area_min, area_max, true);
    const float top = kRowsTop - scroll_;
    if (search_) {
        // the search's own line: what's typed, with a caret while it is
        const ImVec2 min = At(kRowsLeft, top), max = At(kRowsRight, top + kSearchHeight);
        dl->AddRectFilled(min, max, IM_COL32(0, 0, 0, 153));
        dl->AddLine(min, ImVec2(max.x, min.y), IM_COL32(255, 255, 255, 178), Px(2));
        dl->AddLine(ImVec2(min.x, max.y), max, IM_COL32(255, 255, 255, 178), Px(2));
        const float ty = top + (kSearchHeight - 24) * 0.5f;
        PaintText(dl, At(kRowsLeft + 16, ty + 1), kCaps(text::kValue), color::kMuted, "Find");
        const float tw = PaintText(dl, At(kRowsLeft + 84, ty), kPlain(24), color::kText, *search_,
                                   Px(kRowsRight - kRowsLeft - 110));
        if (typing_ && typing_->search && std::fmod(now_, 1.0) < 0.6) {
            const ImVec2 c = At(kRowsLeft + 86, ty + 2);
            dl->AddRectFilled(ImVec2(c.x + tw, c.y), ImVec2(c.x + tw + Px(2), c.y + Px(22)), color::kYellow);
        }
        if (sections_.empty()) {
            float py = top + kSearchHeight + 18;
            Paragraph(dl,
                      search_->empty() ? "Type part of a setting's name or what it does."
                                       : "Nothing matches. Try another word, like \"delay\" or \"video\".",
                      kRowsLeft + 20, py, kRowsRight - kRowsLeft - 40, color::kMuted);
        }
    }
    for (const Placed& p : placed) {
        const float py = top + p.y;
        if (py > kRowsBottom || py + kBandHeight < kRowsTop - kBandHeight) continue;
        if (!p.row) {
            const ImVec2 min = At(kRowsLeft, py), max = At(kRowsRight, py + kBandHeight);
            // a search's band counts its matches
            const std::string right = search_ ? std::to_string(p.section->rows.size()) : std::string();
            DrawBand(dl, min, max, p.section->name, BandKind::kSection, skin_, false, right);
            if (p.section->note) {
                float ny = py + kBandHeight + 4;
                Paragraph(dl, *p.section->note, kRowsLeft + 14, ny, kRowsRight - kRowsLeft - 28,
                          color::kMuted);
            }
            continue;
        }
        const Row& r = *p.row;
        const bool selected = focus_ == Focus::kRows && p.index == row_ && !(typing_ && typing_->search);
        const bool locked = Locked(r);
        const std::string value = RowValue(r);
        const bool typing_here = typing_ && !typing_->search && typing_->row.setting == r.setting &&
                                 typing_->row.index == r.index && typing_->row.kind == r.kind;
        RowLook look;
        look.label = r.label;
        look.value = value;
        look.selected = selected;
        look.locked = locked;
        look.changed = r.kind == RowKind::kControl ? false : RowChanged(model_, r);
        look.error = r.setting && r.kind != RowKind::kControl && problems_.contains(r.setting->cvar);
        // on and off read as a toggle, however the setting stores them
        const bool toggle = r.kind != RowKind::kControl && (value == "On" || value == "Off");
        look.on = toggle && value == "On";
        look.value_caps = toggle || r.kind == RowKind::kControl;
        const bool steps = r.kind == RowKind::kControl
                               ? (r.control == Control::kLightsDevice || r.control == Control::kFog ||
                                  r.control == Control::kStrobe || r.control == Control::kLightsColour ||
                                  r.control == Control::kLeds || r.control == Control::kPattern)
                               : (!Choices(model_, r, devices_.Lists()).empty() || Slider(r) ||
                                  r.kind == RowKind::kLagType ||
                                  (r.setting && (r.setting->widget == Widget::kIntStepper ||
                                                 r.setting->widget == Widget::kFloatInput)));
        look.arrows = selected && steps && !locked && !typing_here;
        const ImVec2 min = At(kRowsLeft, py), max = At(kRowsRight, py + kRowHeight);
        DrawRow(dl, min, max, look, skin_);
        if (typing_here && std::fmod(now_, 1.0) < 0.6) {
            // the caret after the text being typed
            const float x = max.x - Px(20);
            dl->AddRectFilled(ImVec2(x, min.y + Px(9)), ImVec2(x + Px(2), max.y - Px(9)),
                              selected ? color::kOnYellow : color::kYellow);
        }
        if (selected && drop_) drop_->top = py;
        if (!free || !ImGui::IsMouseHoveringRect(min, max) || !ImGui::IsMouseHoveringRect(area_min, area_max)) {
            continue;
        }
        if (MouseMoved(io)) {
            focus_ = Focus::kRows;
            row_ = p.index;
        }
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            focus_ = Focus::kRows;
            row_ = p.index;
            // the arrows either side of the value step it; elsewhere it's A
            const float mx = io.MousePos.x;
            const TextStyle vs = look.value_caps ? kCaps(text::kValue) : kPlain(text::kValue);
            const float vw = std::min(TextWidth(vs, value), (max.x - min.x) * 0.48f);
            const float right = max.x - Px(22 + 10 + 8);
            if (look.arrows && mx > right) {
                Step(r, 1);
            } else if (look.arrows && mx > right - vw - Px(30) && mx < right - vw) {
                Step(r, -1);
            } else {
                Activate(r);
                if (drop_) drop_->top = py;
            }
        }
    }
    dl->PopClipRect();

    // a scrollbar, when the rows don't fit
    if (content > area) {
        const ImVec2 tmin = At(893, kRowsTop), tmax = At(897, kRowsBottom);
        dl->AddRectFilled(tmin, tmax, IM_COL32(255, 255, 255, 20));
        const float h = std::max(area * area / content, 30.0f);
        const float t = kRowsTop + (area - h) * (scroll_ / (content - area));
        dl->AddRectFilled(At(893, t), At(897, t + h), IM_COL32(196, 58, 47, 255), Px(2));
    }
}

void SettingsScreen::Paragraph(ImDrawList* dl, std::string_view text, float x, float& y, float width,
                               ImU32 color) {
    for (const std::string& line : WrapText(kPlain(text::kBody), text, Px(width))) {
        PaintText(dl, At(x, y), kPlain(text::kBody), color, line);
        y += text::kBodyLine;
    }
}

void SettingsScreen::DrawPanel(ImDrawList* dl) {
    ui::DrawPanel(dl, At(kPanelLeft, kPanelTop), At(kPanelRight, kPanelBottom), skin_);
    float y = kBodyTop;
    dl->PushClipRect(At(kPanelLeft, kPanelTop), At(kPanelRight, kBodyBottom + 10), true);
    if (typing_) {
        DrawKeyboard(dl, kBodyLeft, y, kBodyWidth);
    } else if (focus_ == Focus::kCategories && !search_) {
        DrawCategoryDetail(dl, kBodyLeft, y, kBodyWidth);
    } else if (const Row* row = SelectedRow()) {
        DrawRowDetail(dl, *row, kBodyLeft, y, kBodyWidth);
    } else if (search_) {
        Paragraph(dl, "Search finds a setting in any category by its name or what it does.", kBodyLeft, y,
                  kBodyWidth, kBodyText);
    }
    dl->PopClipRect();
}

void SettingsScreen::DrawCategoryDetail(ImDrawList* dl, float x, float& y, float width) {
    const TextStyle heading = kCaps(text::kPanelHeading + 4);
    for (const std::string& line : WrapText(heading, CategoryName(category_), Px(width))) {
        PaintText(dl, At(x, y), heading, color::kText, line);
        y += text::kPanelHeading + 6;
    }
    DrawRule(dl, At(x, y + 6), At(x + width, y + 10), skin_);
    y += 20;
    for (const Section& s : sections_) {
        PaintText(dl, At(x, y + 5), kCaps(text::kBody), color::kMuted, s.name, Px(width));
        dl->AddLine(At(x, y + 30), At(x + width, y + 30), IM_COL32(255, 255, 255, 18));
        y += 30;
    }
    y += 14;
    std::vector<const Row*> changed;
    for (const Section& s : sections_) {
        for (const Row& r : s.rows) {
            if (r.kind != RowKind::kControl && RowChanged(model_, r)) changed.push_back(&r);
        }
    }
    if (changed.empty()) {
        Paragraph(dl, "Everything here is at its default.", x, y, width, color::kMuted);
        return;
    }
    PaintText(dl, At(x, y), kCaps(text::kBody), color::kChanged,
              std::to_string(changed.size()) + " changed from default");
    y += 30;
    for (const Row* r : changed) {
        if (y > kBodyBottom - 28) break;
        const std::string value = RowValue(*r);
        const TextStyle style = kPlain(18);
        const float vw = std::min(TextWidth(style, value), Px(width * 0.45f));
        PaintText(dl, ImVec2(At(x + width, 0).x - vw, At(0, y + 5).y), style, color::kText, value, Px(width * 0.45f));
        PaintText(dl, At(x, y + 5), style, color::kMuted, r->label, Px(width) - vw - Px(12));
        dl->AddLine(At(x, y + 30), At(x + width, y + 30), IM_COL32(255, 255, 255, 18));
        y += 30;
    }
}

void SettingsScreen::DrawRowDetail(ImDrawList* dl, const Row& row, float x, float& y, float width) {
    const ImGuiIO& io = ImGui::GetIO();
    const bool generated = Generated(row);
    const TextStyle heading = generated ? kPlain(text::kPanelHeading) : kCaps(text::kPanelHeading);
    const auto title = WrapText(heading, row.label, Px(width));
    for (size_t i = 0; i < title.size() && i < 3; i++) {
        PaintText(dl, At(x, y), heading, color::kText, title[i], Px(width));
        y += text::kPanelHeading + 4;
    }
    DrawRule(dl, At(x, y + 6), At(x + width, y + 10), skin_);
    y += 22;

    // what it does
    std::string about;
    switch (row.kind) {
    case RowKind::kSetting:
        about = row.setting->about.empty() ? std::string(model_.Description(row.setting->cvar))
                                           : std::string(row.setting->about);
        break;
    case RowKind::kMicSlot:
        about = "The microphone singer " + std::to_string(row.index + 1) +
                " uses. A lists the microphones on this PC.";
        break;
    case RowKind::kLagType:
        about = "Extra delay the game allows " + row.label +
                " controllers, in milliseconds, for one that lags behind the rest. Game's own keeps "
                "its built-in delay.";
        break;
    case RowKind::kControl:
        switch (row.control) {
        case Control::kSettingsFile:
            about = "Every setting here is saved to " +
                    (host_.config_path ? rex::path_to_utf8(host_.config_path()) : std::string("band3.toml")) +
                    ". A opens its folder.";
            break;
        case Control::kLowestDelay: {
            const double hz = DisplayRefreshRate(host_.native_window ? host_.native_window() : nullptr);
            about = "Sets the rows above for the least delay this display allows: a frame rate cap of " +
                    LowestLatencyCap(hz) +
                    (LowestLatencyPacingOff(hz) ? ", with smooth frame pacing off." : ".") +
                    " If the FPS counter's game rate falls short of the cap, lower it.";
            break;
        }
        case Control::kTestControllers:
            about = "Each controller, instrument and MIDI device band3 finds, and a live view of its "
                    "buttons as you press them.";
            break;
        case Control::kVideosList:
            about = "Every song and its music video: find one, sync it with the song, or change its "
                    "offset.";
            break;
        case Control::kSdkSettings:
            about = "The SDK's own settings, the key binds too. Saving there rewrites band3.toml "
                    "with every setting, not only what you changed.";
            break;
        case Control::kLightsDevice: about = "Which Stage Kit the rows below test. All kits tests every one."; break;
        case Control::kLightsCheck: about = "Lights each colour in turn, then turns them off."; break;
        case Control::kFog: about = "Runs the fog machine, or stops it."; break;
        case Control::kStrobe: about = "The strobe, at a speed, now."; break;
        case Control::kLightsColour: about = "The colour the LED and Pattern rows light."; break;
        case Control::kLeds: about = "Lights one LED in the LED colour. Left and right move along."; break;
        case Control::kPattern: about = "Lights a pattern of LEDs in the LED colour."; break;
        case Control::kChase: about = "Odd and even LEDs in turn, in red, green and blue, then off."; break;
        case Control::kLightsOff: about = "Turns every light, the strobe and the fog off."; break;
        }
        break;
    }
    Paragraph(dl, about, x, y, width, kBodyText);

    // its notes
    auto chip = [&](std::string_view text, ChipKind kind) {
        y += 10;
        y += DrawChip(dl, At(x, y), text, kind, Px(width)).y / scale_;
    };
    auto note = [&](std::string_view text, ImU32 color) {
        y += 10;
        const float start = y;
        Paragraph(dl, text, x + 14, y, width - 14, color);
        dl->AddRectFilled(At(x, start + 4), At(x + 4, y - 4), color);
    };
    if (row.setting && row.kind != RowKind::kControl) {
        const Setting& s = *row.setting;
        const std::string cvar(s.cvar);
        if (Locked(row)) chip(LockedWhy(row), ChipKind::kWarn);
        if (const auto it = problems_.find(cvar); it != problems_.end()) note(it->second, color::kBad);
        if (const auto warning = model_.Warning(cvar)) note(*warning, color::kWarn);
        if (!s.note.empty()) note(s.note, color::kWarn);
        if (host_.where == Where::kInGame && !model_.ReadOnly(cvar) &&
            (model_.NextStartOnly(cvar) || model_.WaitsForNextStart(cvar))) {
            chip("Applies when band3 restarts", ChipKind::kInfo);
        }
        if (host_.where == Where::kLauncher &&
            ((cvar == "input_backend" && input::InputBackendChanged()) ||
             (cvar == "renderer" && model_.RendererNeedsRestart()))) {
            chip("Play restarts band3 for this", ChipKind::kInfo);
        }
        if (cvar == "game_data_root" && !model_.ReadOnly(cvar)) {
            if (check_.ok) {
                chip("Rock Band 3 found here", ChipKind::kGood);
            } else {
                note(std::string(DescribeProblem(check_.problem)) + ": " +
                         (checked_root_.empty() ? std::string("no folder set")
                                                : rex::path_to_utf8(checked_root_)),
                     color::kBad);
            }
        }
        if ((s.widget == Widget::kPath || s.widget == Widget::kFolderList) && row.kind == RowKind::kSetting) {
            const std::string value = model_.Value(cvar);
            const std::string shown = value.empty()
                                          ? rex::path_to_utf8(std::filesystem::path(model_.EffectiveDefault(cvar)).make_preferred())
                                          : value;
            if (!shown.empty() && s.widget == Widget::kPath) {
                y += 8;
                Paragraph(dl, shown, x, y, width, color::kMuted);
            }
        }
        if (RowChanged(model_, row) && !IsShowLauncher(row) && !Locked(row)) {
            chip(keyboard_hints_ ? "Changed: R resets it" : "Changed: Y resets it", ChipKind::kChanged);
        }
        // the mic's level, on the launcher
        if (row.kind == RowKind::kMicSlot && FeaturesFor(host_.where).mic_meters) {
            std::vector<std::string> slots = audio::usb_mic::ParseDeviceList(model_.Value(cvar));
            slots.resize(audio::usb_mic::kSingers);
            const std::string& slot = slots[size_t(row.index)];
            const bool all_empty = std::ranges::all_of(slots, &std::string::empty);
            std::optional<std::string> device;
            if (row.index == 0 && all_empty && !devices_.Lists().mics.empty()) device = "";
            else if (!slot.empty() && FindSavedDevice(devices_.Lists().mics, slot)) device = slot;
            if (const auto meter = devices_.MeterFor(row.index, device)) {
                y += 14;
                if (!meter->recording) {
                    note("Can't record: " + meter->error, color::kBad);
                } else {
                    const float floor = -60;
                    auto pos = [&](float amplitude) {
                        return (MicLevel::ToDecibels(amplitude, floor) - floor) / -floor;
                    };
                    const ImVec2 bmin = At(x, y), bmax = At(x + width, y + 10);
                    dl->AddRectFilled(bmin, bmax, IM_COL32(255, 255, 255, 26), Px(5));
                    const float rms = pos(meter->level.rms);
                    if (rms > 0) {
                        const float db = MicLevel::ToDecibels(meter->level.rms, floor);
                        const ImU32 c = db > -6 ? color::kBad : db > -18 ? color::kWarn : color::kGood;
                        dl->AddRectFilled(bmin, ImVec2(bmin.x + std::max((bmax.x - bmin.x) * rms, Px(10)), bmax.y), c,
                                          Px(5));
                    }
                    const float peak = pos(meter->level.peak);
                    if (peak > 0) {
                        const float px = bmin.x + std::clamp((bmax.x - bmin.x) * peak, Px(1), bmax.x - bmin.x - Px(1));
                        dl->AddLine(ImVec2(px, bmin.y - Px(3)), ImVec2(px, bmax.y + Px(3)), color::kText, Px(2));
                    }
                    y += 16;
                    PaintText(dl, At(x, y), kCaps(text::kChip), color::kMuted, "Level now");
                    y += 22;
                }
            }
        }
        if (generated) {
            y += 10;
            PaintText(dl, At(x, y), kPlain(17), IM_COL32(138, 144, 161, 255), cvar + " in band3.toml", Px(width));
            y += 22;
        }
    } else if (row.kind == RowKind::kControl) {
        if (row.control == Control::kLowestDelay && !undo_.empty()) {
            y += 12;
            PaintText(dl, At(x, y), kCaps(19), color::kGood, "Just changed");
            y += 28;
            for (const auto& [cvar, old] : undo_) {
                const Setting* s = model_.Find(cvar);
                if (!s) continue;
                const Row r{.kind = RowKind::kSetting, .setting = s, .label = std::string(s->label)};
                PaintText(dl, At(x, y), kPlain(18), color::kMuted, r.label, Px(width * 0.55f));
                const std::string now = RowValue(r);
                const float vw = TextWidth(kPlain(18), now);
                PaintText(dl, ImVec2(At(x + width, 0).x - vw, At(0, y).y), kPlain(18), color::kText, now);
                y += 26;
            }
        }
        if (row.control >= Control::kLightsDevice && row.control <= Control::kLightsOff) {
            if (const std::string problem = lights::PicoProblem(); !problem.empty()) note(problem, color::kWarn);
            if (lights::PicosNeedEvents()) {
                note("Wireless kits follow the game through the RB3E events, which are off: turn on "
                     "Send events in Other Apps.",
                     color::kWarn);
            }
            const std::vector<lights::DeviceRow> devices = lights::Devices();
            y += 12;
            if (devices.empty()) {
                Paragraph(dl, "No Stage Kits found yet. Plug one into this PC, or switch on a Pico W kit on this "
                              "network.",
                          x, y, width, color::kMuted);
            }
            for (const lights::DeviceRow& d : devices) {
                if (y > kBodyBottom - 40) break;
                PaintText(dl, At(x, y), kPlain(19), color::kText, d.kind + "  " + d.name, Px(width));
                y += 22;
                PaintText(dl, At(x, y), kPlain(16), d.online ? color::kMuted : color::kWarn, d.detail, Px(width));
                y += 24;
            }
            if (const std::string last = lights::LastSent(); !last.empty()) {
                y += 6;
                Paragraph(dl, "Sent: " + last, x, y, width, color::kMuted);
            }
        }
    }

    // the choices, the current one marked; each can be clicked
    if (Locked(row) || OpensList(row) || row.kind == RowKind::kControl) return;
    const std::vector<ChoiceItem> choices = Choices(model_, row, devices_.Lists());
    if (choices.size() < 2 || IsShowLauncher(row)) return;
    y += 14;
    const bool free = !box_ && !typing_ && !drop_;
    for (const ChoiceItem& c : choices) {
        if (y + 28 > kBodyBottom) break;
        const ImVec2 min = At(x, y), max = At(x + width, y + 28);
        if (c.current) {
            dl->AddRectFilledMultiColor(min, max, IM_COL32(230, 220, 31, 56), IM_COL32(230, 220, 31, 0),
                                        IM_COL32(230, 220, 31, 0), IM_COL32(230, 220, 31, 56));
            dl->AddRectFilled(ImVec2(min.x, min.y + Px(4)), ImVec2(min.x + Px(4), max.y - Px(4)), color::kYellow);
        }
        const bool hovered = free && ImGui::IsMouseHoveringRect(min, max);
        float tag_w = 0;
        if (c.is_default) {
            tag_w = TextWidth(kCaps(text::kChip), "Default");
            PaintText(dl, ImVec2(max.x - Px(8) - tag_w, min.y + Px(6)), kCaps(text::kChip),
                      IM_COL32(138, 144, 161, 255), "Default");
        }
        PaintText(dl, ImVec2(min.x + Px(12), min.y + Px(4)), kPlain(text::kBody),
                  c.current || hovered ? color::kText : IM_COL32(195, 199, 210, 255), c.label,
                  max.x - min.x - Px(24) - tag_w);
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) Apply(row, c.value);
        y += 28;
    }
    (void)io;
}

void SettingsScreen::DrawKeyboard(ImDrawList* dl, float x, float& y, float width) {
    const std::string heading = typing_->search ? "Search settings" : typing_->row.label;
    const OnScreenKeyboard::Result result = DrawKeyboardPanel(
        dl, Page{origin_, scale_}, keyboard_,
        KeyboardPanel{.heading = heading,
                      .hint = typing_->search ? "Results update as you type. Done goes to them."
                                              : "A keyboard plugged in types too.",
                      .secret = typing_->secret,
                      .x = x,
                      .y = y,
                      .width = width},
        skin_, !box_ && Clicked());
    if (result != OnScreenKeyboard::Result::kNone || (typing_->search && Clicked())) HandleTyping(result);
}
void SettingsScreen::DrawDrop(ImDrawList* dl) {
    const ImGuiIO& io = ImGui::GetIO();
    DropList& drop = *drop_;
    const int count = int(drop.items.size());
    const int shown = std::min(count, kDropVisible);
    const float h = float(shown) * kDropItem + 50;
    float top = std::clamp(drop.top - 14, 40.0f, kBodyBottom + 22 - h);
    const ImVec2 min = At(kDropLeft, top), max = At(kDropLeft + kDropWidth, top + h);
    ui::DrawPanel(dl, min, max, skin_);
    const bool free = !box_;
    if (free && ImGui::IsMouseHoveringRect(min, max) && io.MouseWheel != 0) {
        drop.scroll = std::clamp(drop.scroll - int(io.MouseWheel), 0, std::max(0, count - shown));
    }
    float y = top + 26;
    for (int i = drop.scroll; i < drop.scroll + shown && i < count; i++) {
        const ChoiceItem& item = drop.items[size_t(i)];
        const ImVec2 imin = At(kDropLeft + 22, y), imax = At(kDropLeft + kDropWidth - 22, y + kDropItem);
        const bool selected = i == drop.selected;
        if (selected) DrawHighlight(dl, imin, imax, skin_, Px(3));
        if (item.current) dl->AddRectFilled(ImVec2(imin.x, imin.y + Px(6)), ImVec2(imin.x + Px(4), imax.y - Px(6)),
                                            selected ? color::kOnYellow : color::kYellow);
        float tag_w = 0;
        if (item.is_default) {
            tag_w = TextWidth(kCaps(text::kChip), "Default") + Px(12);
            PaintText(dl, ImVec2(imax.x - tag_w, imin.y + Px(9)), kCaps(text::kChip),
                      selected ? color::kOnYellow : IM_COL32(159, 166, 184, 255), "Default");
        }
        PaintText(dl, ImVec2(imin.x + Px(14), imin.y + Px(6)), kPlain(22), selected ? color::kOnYellow : color::kText,
                  item.label, imax.x - imin.x - Px(20) - tag_w);
        if (free && ImGui::IsMouseHoveringRect(imin, imax)) {
            if (MouseMoved(io)) drop.selected = i;
            if (Clicked()) {
                PickDrop(i);
                return;
            }
        }
        y += kDropItem;
    }
    if (count > shown) {
        // where the list is
        const float track = float(shown) * kDropItem;
        const float th = std::max(track * float(shown) / float(count), 20.0f);
        const float ty = top + 26 + (track - th) * float(drop.scroll) / float(count - shown);
        dl->AddRectFilled(At(kDropLeft + kDropWidth - 16, ty), At(kDropLeft + kDropWidth - 13, ty + th),
                          IM_COL32(196, 58, 47, 255), Px(2));
    }
    // a click outside closes it
    if (free && Clicked() && !ImGui::IsMouseHoveringRect(min, max)) {
        drop_.reset();
    }
}

void SettingsScreen::DrawHelp(ImDrawList* dl, ImGuiIO& io) {
    const Row* row = SelectedRow();
    std::vector<HelpItem> items;
    if (box_) {
        items = {{HelpButton::kA, "Select"}};
        if (box_->cancel >= 0) items.push_back({HelpButton::kB, "Back"});
    } else if (typing_) {
        items = {{HelpButton::kA, "Type"}, {HelpButton::kB, "Cancel"}, {HelpButton::kX, "Delete"},
                 {HelpButton::kY, "Space"}};
    } else if (drop_) {
        items = {{HelpButton::kA, "Pick"}, {HelpButton::kB, "Cancel"}};
    } else if (focus_ == Focus::kCategories && !search_) {
        items = {{HelpButton::kA, "Open"},
                 {HelpButton::kB, host_.where == Where::kInGame ? "Back to pause" : "Back to menu"},
                 {HelpButton::kX, "Search"}};
    } else {
        const bool undo = row && row->kind == RowKind::kControl && row->control == Control::kLowestDelay &&
                          !undo_.empty();
        items = {{HelpButton::kA, "Change"},
                 {HelpButton::kB, search_ ? "Close search" : "Back"},
                 {HelpButton::kY, undo ? "Undo" : "Reset"},
                 {HelpButton::kX, search_ ? "Type more" : "Search"}};
    }
    const bool play = host_.where == Where::kLauncher && !typing_ && !box_;
    const int hovered = DrawHelpBar(dl, ImVec2(0, At(0, kHelpTop).y), ImVec2(io.DisplaySize.x, io.DisplaySize.y),
                                    items, keyboard_hints_, play, skin_);
    if (hovered < 0 || !ImGui::IsMouseClicked(ImGuiMouseButton_Left)) return;
    if (hovered == kHelpPlay) {
        Handle(Action::kPlay);
        return;
    }
    switch (items[size_t(hovered)].button) {
    case HelpButton::kA: Handle(Action::kSelect); break;
    case HelpButton::kB: Handle(Action::kBack); break;
    case HelpButton::kX: Handle(Action::kSecondary); break;
    case HelpButton::kY: Handle(Action::kReset); break;
    case HelpButton::kBumpers: Handle(Action::kNextCategory); break;
    case HelpButton::kStart: Handle(Action::kPlay); break;
    }
}

}
