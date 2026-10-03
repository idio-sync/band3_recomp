#include "launcher_dialog.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include "src/Audio/usb_mic.h"
#include "src/Input/input_system.h"
#include "src/paths.h"
#include "src/settings.h"
#include "launcher_start.h"
#include "launcher_style.h"

namespace band3::launcher {


struct FolderPick {
    std::mutex mutex;
    bool done = false;
    std::optional<std::string> folder;
    // the dialog couldn't be shown
    std::string error;
};

namespace {

constexpr const char* kQuitPrompt = "Quit without saving?";
constexpr const char* kPlayPrompt = "Play anyway?";
constexpr const char* kSaveFailedPrompt = "Settings not saved";

// the footer's description: the setting's name and three lines of small text
float DescriptionHeight() { return Px(kSmallSize) * 4 + Px(2); }

// a button's width, for lining up a row of them on the right
float ButtonWidth(const char* label) {
    return ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2;
}

std::string Str(std::string_view s) { return std::string(s); }

std::string FormatNumber(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
}

// a box across the page: Steam Deck, band3.toml problems
template <typename Body>
void Banner(const char* id, const ImVec4& background, Body&& body) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(16), Px(10)));
    if (ImGui::BeginChild(id, ImVec2(0, 0),
                          ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar)) {
        body();
    }
    ImGui::EndChild();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
}

int ResizeText(ImGuiInputTextCallbackData* data) {
    if (data->EventFlag == ImGuiInputTextFlags_CallbackResize) {
        auto* text = static_cast<std::vector<char>*>(data->UserData);
        text->resize(static_cast<size_t>(data->BufSize));
        data->Buf = text->data();
    }
    return 0;
}

void SDLCALL OnFolderPicked(void* userdata, const char* const* files, int) {
    auto* holder = static_cast<std::shared_ptr<FolderPick>*>(userdata);
    {
        FolderPick& pick = **holder;
        std::lock_guard lock(pick.mutex);
        pick.done = true;
        if (!files) {
            pick.error = SDL_GetError();
        } else if (files[0]) {
            pick.folder = files[0];
        }
    }
    delete holder;
}

// what an empty text setting means, shown greyed in its field
const char* HintFor(std::string_view cvar) {
    if (cvar == "username") return "The profile's own name";
    if (cvar == "midi_drums_device") return "A port's name, or part of one";
    if (cvar == "usb_mic_devices") return "A microphone's name, or part of one";
    if (cvar == "resolution") return "Width x height, e.g. 1600x900";
    if (cvar == "forced_venue") return "A venue or a comma separated list";
    return "";
}

constexpr std::pair<Tab, const char*> kTabs[] = {
    {Tab::kGame, "Game"},
    {Tab::kGraphics, "Graphics"},
    {Tab::kAudio, "Audio"},
    {Tab::kControllers, "Controllers"},
    {Tab::kOnline, "Online"},
};

}

LauncherDialog::LauncherDialog(rex::ui::ImGuiDrawer* imgui_drawer, LauncherHost host)
    : rex::ui::ImGuiDialog(imgui_drawer),
      host_(std::move(host)),
      model_(SettingTable(), ReadEnvironment(SettingTable(), host_.path_defaults, host_.anchor),
             store_) {
    // the SDK's ImGui has no navigation; the launcher can be driven from the
    // keyboard and controllers until it closes
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    nav_.emplace(io);
    config_problem_ = ConfigFileProblem(host_.config_path);
    if (config_problem_) {
        REXLOG_WARN("Launcher: {} couldn't be read: {}", rex::path_to_utf8(host_.config_path),
                    *config_problem_);
    }
}

LauncherDialog::~LauncherDialog() {
    CloseMeters();
    nav_.reset();
    ImGuiIO& io = GetIO();
    io.ConfigFlags = (io.ConfigFlags & ~ImGuiConfigFlags_NavEnableKeyboard) |
                     (saved_config_flags_ & ImGuiConfigFlags_NavEnableKeyboard);
}

bool LauncherDialog::HasUnsavedChanges() const { return model_.HasUnsavedChanges(); }

void LauncherDialog::RequestQuit() {
    if (stage_ != Stage::kEditing) return;
    if (HasUnsavedChanges()) {
        open_quit_prompt_ = true;
        return;
    }
    if (host_.quit) host_.quit();
}

void LauncherDialog::Pace() {
    using Clock = std::chrono::steady_clock;
    Clock::time_point now = Clock::now();
    if (now >= next_refresh_query_) {
        const double hz = host_.refresh_rate ? host_.refresh_rate() : 0.0;
        refresh_hz_ = hz >= 24.0 ? std::min(hz, 360.0) : 60.0;
        next_refresh_query_ = now + std::chrono::seconds(1);
    }
    const auto period = std::chrono::duration_cast<Clock::duration>(
        std::chrono::duration<double>(1.0 / refresh_hz_));
    const Clock::time_point due = last_frame_ + period;
    if (now < due) {
        std::this_thread::sleep_until(due);
        // from when it was due, so oversleeping doesn't slow the next frame
        last_frame_ = due;
    } else {
        last_frame_ = now;
    }
}

void LauncherDialog::OnDraw(ImGuiIO& io) {
    // before the game runs nothing else holds the frame rate down: the SDK
    // waits for the display's vblank only once it presents the game, and not
    // at all without a display (a disconnected remote session)
    Pace();

    ImGuiStyle& style = ImGui::GetStyle();
    const ImGuiStyle sdk_style = style;
    SetScale(ScaleFor(io.DisplaySize));
    ApplyStyle(style, Scale());
    {
        FontScope font(kBodySize);
        if (stage_ == Stage::kEditing) {
#ifndef _WIN32
            // the folder dialog goes through the xdg desktop portal off
            // Windows, which may answer only while SDL's events are pumped,
            // and nothing pumps them before the game runs. Unverified on
            // Linux; harmless without video.
            if (pick_) SDL_PumpEvents();
#endif
            TakeFolderPick();
            RefreshGameDataCheck(false);
            // the instrument settings restart band3's drivers as they change
            input::ApplyInputSettings();
            // the devices are read only until Play: then they're the game's
            device_panel_.Poll(current_tab_ == Tab::kControllers);
            HandleNav(nav_->Feed(device_panel_.NavPads()));
            meters_drawn_ = false;
            DrawPage(io);
            // the meters record only while their slots show: not on another
            // tab, nor with usb_mics off
            if (!meters_drawn_) CloseMeters();
        } else {
            // nothing of the launcher's records once Play is pressed: the
            // game's own capture opens the microphones
            CloseMeters();
            device_panel_.StopTest();
            nav_->Stop();
            DrawStarting(io);
        }
    }
    style = sdk_style;

    // the frame just drawn shows "Starting" while the runtime is built, which
    // holds up the UI thread for a while; the next one starts the game
    if (stage_ == Stage::kStarting && ++starting_frames_ > 1) {
        stage_ = Stage::kStarted;
        if (host_.start_game) host_.start_game(restart_);
    }
}

void LauncherDialog::DrawPage(ImGuiIO& io) {
    hovered_ = std::move(hovered_next_);
    hovered_next_.clear();

    // covers the window, under any overlay opened from it (F4, the console)
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher", nullptr, flags)) {
        DrawHeader();
        DrawBanners();

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(Px(18), Px(8)));
        if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_NoTooltip)) {
            for (const auto& [tab, name] : kTabs) {
                const ImGuiTabItemFlags select =
                    pending_tab_ == tab ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
                if (ImGui::BeginTabItem(name, nullptr, select)) {
                    current_tab_ = tab;
                    ImGui::EndTabItem();
                }
            }
            pending_tab_.reset();
            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar();
        RefreshDeviceLists();
        DrawDeckBanner();
        ImGui::Dummy(ImVec2(0, Px(2)));

        // the footer keeps its place at the bottom; the settings scroll above it
        const ImGuiStyle& style = ImGui::GetStyle();
        const float footer_height = Px(8) + DescriptionHeight() + ImGui::GetFrameHeight() +
                                    style.ItemSpacing.y * 4;
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
        // (in a window too short for both, the settings keep a few rows)
        const float settings_height =
            std::max(ImGui::GetContentRegionAvail().y - footer_height, Px(80));
        // (flattened: the keyboard and controllers move between the settings
        // and the footer as if they were one page)
        if (ImGui::BeginChild("##settings", ImVec2(0, settings_height),
                              ImGuiChildFlags_NavFlattened)) {
            ImGui::Dummy(ImVec2(0, Px(4)));
            DrawTab(current_tab_);
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        DrawFooter();
        DrawPrompts();
    }
    ImGui::End();
}

void LauncherDialog::DrawHeader() {
    {
        FontScope font(kTitleSize);
        ImGui::TextUnformatted("band3");
    }
    ImGui::SameLine();
    {
        // on the title's baseline
        FontScope font(kHeadingSize);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(kTitleSize - kHeadingSize) * 0.8f);
        ImGui::TextColored(kMuted, "setup");
    }
    // where the settings go, on the right, if there's room
    const float title_end = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x;
    FontScope font(kSmallSize);
    const std::string where = "Saves to " + rex::path_to_utf8(host_.config_path);
    const float width = ImGui::CalcTextSize(where.c_str()).x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    if (right - width > title_end + Px(40)) {
        ImGui::SameLine(right - width);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + Px(kTitleSize - kSmallSize) * 0.8f);
        ImGui::TextColored(kMuted, "%s", where.c_str());
    }
    ImGui::Dummy(ImVec2(0, Px(2)));
}

void LauncherDialog::DrawBanners() {
    if (!config_problem_) return;
    Banner("##config_problem", kProblemBanner, [&] {
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kWarn, "band3.toml couldn't be read:");
        ImGui::SameLine();
        ImGui::TextUnformatted(config_problem_->c_str());
        ImGui::TextUnformatted(
            "Saving replaces it, and the old file is kept as band3.toml.bak.");
        ImGui::PopTextWrapPos();
    });
    ImGui::Spacing();
}

void LauncherDialog::DrawDeckBanner() {
    const Setting* deck = model_.Find("steam_deck_defaults");
    if (!deck || !model_.Visible(*deck)) return;
    ImGui::Spacing();
    Banner("##steam_deck", kDeckBanner, [&] {
        ImGui::PushID("steam_deck_defaults");
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Steam Deck");
        ImGui::SameLine(0, Px(24));
        const bool locked = model_.IsLocked(deck->cvar);
        ImGui::BeginDisabled(locked);
        bool on = AsBool(model_.Value(deck->cvar));
        if (ImGui::Checkbox(Str(deck->label).c_str(), &on)) {
            model_.Set(deck->cvar, on ? "true" : "false");
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) NoteHovered(deck->cvar);
        ImGui::SameLine(0, Px(24));
        if (locked) {
            ImGui::TextColored(kMuted, "%s", LockReason(model_.LockOf(deck->cvar)));
        } else {
            ImGui::TextColored(kMuted, on ? "Settings that suit the Deck are the defaults; "
                                            "what you change here wins over them."
                                          : "band3's desktop defaults.");
            if (model_.IsChanged(deck->cvar)) {
                ImGui::SameLine();
                if (ImGui::Button("Reset")) model_.Reset(deck->cvar);
            }
        }
        ImGui::PopID();
    });
}

void LauncherDialog::DrawTab(Tab tab) {
    if (tab == Tab::kControllers) device_panel_.Draw();
    for (const std::string_view section : SectionsOf(model_.Table(), tab)) {
        DrawSection(tab, section);
    }
}

void LauncherDialog::DrawSection(Tab tab, std::string_view section) {
    std::vector<const Setting*> rows;
    for (const auto& s : model_.Table()) {
        if (s.tab == tab && s.section == section && model_.Visible(s)) rows.push_back(&s);
    }
    if (rows.empty()) return;

    const std::string name(section);
    SectionHeading(name.c_str());

    const float width = ImGui::GetContentRegionAvail().x;
    const float label_width = std::clamp(width * 0.36f, Px(220), Px(420));
    const float reset_width = ButtonWidth("Reset");
    const ImGuiTableFlags flags = ImGuiTableFlags_None;
    if (ImGui::BeginTable(name.c_str(), 3, flags)) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed, label_width);
        ImGui::TableSetupColumn("control", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("reset", ImGuiTableColumnFlags_WidthFixed, reset_width);
        for (const Setting* s : rows) DrawRow(*s);
        ImGui::EndTable();
    }
    ImGui::Dummy(ImVec2(0, Px(14)));
}

void LauncherDialog::DrawRow(const Setting& setting) {
    ImGui::PushID(Str(setting.cvar).c_str());
    ImGui::TableNextRow();

    ImGui::TableSetColumnIndex(0);
    ImGui::AlignTextToFramePadding();
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x);
    ImGui::TextUnformatted(Str(setting.label).c_str());
    ImGui::PopTextWrapPos();
    if (ImGui::IsItemHovered()) NoteHovered(setting.cvar);

    const bool locked =
        model_.IsLocked(setting.cvar) ||
        (!setting.companion.empty() && model_.IsLocked(setting.companion));
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(locked);
    DrawControl(setting);
    ImGui::EndDisabled();
    if (locked) {
        const Lock lock = model_.IsLocked(setting.cvar) ? model_.LockOf(setting.cvar)
                                                        : model_.LockOf(setting.companion);
        FontScope font(kSmallSize);
        ImGui::TextColored(kMuted, "%s; it can't be changed here.", LockReason(lock));
    }
    // the input system can't be swapped while band3 runs
    if (setting.cvar == "input_backend" && input::InputBackendChanged()) {
        FontScope font(kSmallSize);
        ImGui::TextColored(kMuted, "Applies when you press Play (band3 restarts)");
    }
    if (const auto warning = model_.Warning(setting.cvar)) {
        FontScope font(kSmallSize);
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(warning->c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    if (const auto problem = row_problems_.find(setting.cvar); problem != row_problems_.end()) {
        FontScope font(kSmallSize);
        ImGui::PushStyleColor(ImGuiCol_Text, kBad);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(problem->second.c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ||
        NavCursorWithin(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
        NoteHovered(setting.cvar);
    }

    ImGui::TableSetColumnIndex(2);
    if (!locked && model_.IsChanged(setting.cvar)) {
        if (ImGui::Button("Reset") && model_.Reset(setting.cvar)) {
            row_problems_.erase(Str(setting.cvar));
        }
        if (ImGui::IsItemHovered()) NoteHovered(setting.cvar);
    }
    ImGui::PopID();
}

void LauncherDialog::DrawControl(const Setting& setting) {
    switch (setting.widget) {
    case Widget::kCheckbox: DrawCheckbox(setting); break;
    case Widget::kCombo: DrawCombo(setting); break;
    case Widget::kComboText: DrawComboText(setting); break;
    case Widget::kIntStepper: DrawIntStepper(setting); break;
    case Widget::kIntSlider: DrawIntSlider(setting); break;
    case Widget::kFloatSlider: DrawFloatSlider(setting, false); break;
    case Widget::kPercentSlider: DrawFloatSlider(setting, true); break;
    case Widget::kText: DrawText(setting); break;
    case Widget::kPath: DrawPath(setting); break;
    case Widget::kFolderList: DrawFolderList(setting); break;
    case Widget::kMicSlots: DrawMicSlots(setting); break;
    case Widget::kMidiPort: DrawMidiPort(setting); break;
    case Widget::kMonitor: DrawMonitor(setting); break;
    case Widget::kResolution: DrawResolution(setting); break;
    case Widget::kJoypadLag: DrawJoypadLag(setting); break;
    case Widget::kWindowMode: DrawWindowMode(setting); break;
    case Widget::kNone: break;
    }
}

namespace {

// how wide a dropdown or slider is: a comfortable width, or the column
float ControlWidth() { return std::min(ImGui::GetContentRegionAvail().x, Px(380)); }

}

void LauncherDialog::DrawCheckbox(const Setting& s) {
    bool on = AsBool(model_.Value(s.cvar));
    if (ImGui::Checkbox("##value", &on)) Apply(s.cvar, on ? "true" : "false");
}

void LauncherDialog::DrawCombo(const Setting& s) {
    const int index = model_.ChoiceIndex(s);
    const std::string custom = model_.Value(s.cvar) + " (set elsewhere)";
    const std::string preview = index >= 0 ? Str(s.choices[index].label) : custom;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        for (size_t i = 0; i < s.choices.size(); i++) {
            const bool selected = static_cast<int>(i) == index;
            if (ImGui::Selectable(Str(s.choices[i].label).c_str(), selected)) {
                Apply(s.cvar, s.choices[i].value);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        if (index < 0) {
            ImGui::Selectable(custom.c_str(), true);
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void LauncherDialog::DrawComboText(const Setting& s) {
    const int index = model_.ChoiceIndex(s);
    const bool custom = index < 0 || custom_rows_.contains(s.cvar);
    const char* kCustom = "Other...";
    const std::string preview = custom ? kCustom : Str(s.choices[index].label);
    const float avail = ImGui::GetContentRegionAvail().x;
    const float combo_width = custom ? std::min(avail * 0.45f, Px(240)) : ControlWidth();
    ImGui::SetNextItemWidth(combo_width);
    if (ImGui::BeginCombo("##choice", preview.c_str())) {
        for (size_t i = 0; i < s.choices.size(); i++) {
            const bool selected = !custom && static_cast<int>(i) == index;
            if (ImGui::Selectable(Str(s.choices[i].label).c_str(), selected)) {
                custom_rows_.erase(Str(s.cvar));
                Apply(s.cvar, s.choices[i].value);
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        if (ImGui::Selectable(kCustom, custom)) custom_rows_.insert(Str(s.cvar));
        if (custom) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
    if (!custom) return;
    ImGui::SameLine();
    std::string value = model_.Value(s.cvar);
    if (EditText("##custom", HintFor(s.cvar), value, ImGui::GetContentRegionAvail().x)) {
        Apply(s.cvar, value);
    }
}

void LauncherDialog::DrawIntStepper(const Setting& s) {
    const std::string value = model_.Value(s.cvar);
    int v = static_cast<int>(AsInt(value).value_or(0));
    const Range range = s.range.value_or(Range{0, 0, 1});
    const int step = std::max(1, static_cast<int>(range.step));
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, Px(210)));
    if (ImGui::InputInt("##value", &v, step, step * 10)) {
        if (range.max > range.min) {
            v = std::clamp(v, static_cast<int>(range.min), static_cast<int>(range.max));
        }
        Apply(s.cvar, std::to_string(v));
    }
    // a special value's name, or the unit
    const int index = model_.ChoiceIndex(s);
    if (index >= 0) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", Str(s.choices[index].label).c_str());
    } else if (!s.unit.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "%s", Str(s.unit).c_str());
    }
}

void LauncherDialog::DrawIntSlider(const Setting& s) {
    int v = static_cast<int>(AsInt(model_.Value(s.cvar)).value_or(0));
    const Range range = s.range.value_or(Range{0, 100, 1});
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderInt("##value", &v, static_cast<int>(range.min), static_cast<int>(range.max))) {
        Apply(s.cvar, std::to_string(v));
    }
}

void LauncherDialog::DrawFloatSlider(const Setting& s, bool percent) {
    const Range range = s.range.value_or(Range{0, 1, 0.01});
    const double value = AsFloat(model_.Value(s.cvar)).value_or(range.min);
    const double shown_scale = percent ? 100.0 : 1.0;
    float v = static_cast<float>(value * shown_scale);
    const std::string format = percent ? "%.0f%%" : "%.2f" + Str(s.unit);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderFloat("##value", &v, static_cast<float>(range.min * shown_scale),
                           static_cast<float>(range.max * shown_scale), format.c_str())) {
        double next = v / shown_scale;
        // on the range's steps, so a drag lands on 1.25 rather than 1.2493
        if (range.step > 0) next = std::round(next / range.step) * range.step;
        Apply(s.cvar, FormatNumber(std::clamp(next, range.min, range.max)));
    }
}

void LauncherDialog::DrawText(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    if (EditText("##value", HintFor(s.cvar), value, ImGui::GetContentRegionAvail().x)) {
        Apply(s.cvar, value);
    }
}

void LauncherDialog::DrawPath(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    const std::string fallback =
        rex::path_to_utf8(std::filesystem::path(model_.EffectiveDefault(s.cvar)).make_preferred());
    const std::string hint = fallback.empty() ? std::string() : "Default: " + fallback;
    const float browse = ButtonWidth("Browse...");
    const float field = ImGui::GetContentRegionAvail().x - browse - ImGui::GetStyle().ItemSpacing.x;
    if (EditText("##value", hint.c_str(), value, field)) Apply(s.cvar, value);
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        const std::string from = value.empty()
                                     ? fallback
                                     : rex::path_to_utf8(paths::Resolve(value, model_.Env().anchor));
        StartFolderPick(Str(s.cvar), from);
    }
    if (s.cvar == "game_data_root") {
        DrawGameDataCheck();
    } else if (!value.empty() && std::filesystem::path(value).is_relative()) {
        FontScope font(kSmallSize);
        const std::string resolved =
            rex::path_to_utf8(paths::Resolve(value, model_.Env().anchor));
        ImGui::TextColored(kMuted, "%s", resolved.c_str());
    }
}

void LauncherDialog::RefreshGameDataCheck(bool now) {
    using Clock = std::chrono::steady_clock;
    const std::filesystem::path root =
        host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (!now && check_done_ && root == checked_root_ && Clock::now() < next_check_) return;
    checked_root_ = root;
    check_ = CheckGameData(root);
    check_done_ = true;
    next_check_ = Clock::now() + std::chrono::seconds(1);
}

void LauncherDialog::DrawGameDataCheck() {
    FontScope font(kSmallSize);
    const std::string shown =
        checked_root_.empty() ? "(not set)" : rex::path_to_utf8(checked_root_);
    ImGui::PushTextWrapPos(0);
    if (check_.ok) {
        ImGui::TextColored(kGood, "Found Rock Band 3 in %s", shown.c_str());
    } else {
        ImGui::TextColored(kWarn, "%s (%s)", DescribeProblem(check_.problem), shown.c_str());
    }
    ImGui::PopTextWrapPos();
}

bool LauncherDialog::FolderExists(const std::filesystem::path& folder) {
    using Clock = std::chrono::steady_clock;
    auto [it, added] = folder_states_.try_emplace(folder);
    FolderState& state = it->second;
    if (added || Clock::now() - state.checked >= std::chrono::seconds(3)) {
        std::error_code ec;
        state.exists = std::filesystem::is_directory(folder, ec);
        state.checked = Clock::now();
    }
    return state.exists;
}

void LauncherDialog::DrawFolderList(const Setting& s) {
    const std::string value = model_.Value(s.cvar);
    if (value != folder_states_for_) {
        folder_states_.clear();
        folder_states_for_ = value;
    }
    std::vector<std::string> folders = paths::SplitList(value);
    auto join = [](const std::vector<std::string>& list) {
        std::string out;
        for (const auto& folder : list) {
            if (!out.empty()) out += '|';
            out += folder;
        }
        return out;
    };

    // the default's folders (songs beside the ini) aren't there until someone
    // makes them: band3 ships without one
    const std::vector<std::string> defaults = paths::SplitList(model_.EffectiveDefault(s.cvar));
    const float remove = ButtonWidth("Remove");
    std::optional<size_t> removed;
    for (size_t i = 0; i < folders.size(); i++) {
        ImGui::PushID(static_cast<int>(i));
        std::filesystem::path resolved =
            paths::Resolve(folders[i], model_.Env().anchor).lexically_normal();
        // "." is the anchor itself, without a trailing separator
        if (!resolved.has_filename() && resolved.has_relative_path()) {
            resolved = resolved.parent_path();
        }
        const bool exists = FolderExists(resolved);
        ImGui::AlignTextToFramePadding();
        const float text_width = ImGui::GetContentRegionAvail().x - remove -
                                 ImGui::GetStyle().ItemSpacing.x;
        const float start = ImGui::GetCursorPosX();
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                            ImVec2(ImGui::GetCursorScreenPos().x + text_width,
                                   ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight()),
                            true);
        ImGui::TextUnformatted(rex::path_to_utf8(resolved).c_str());
        const bool default_folder = std::ranges::find(defaults, folders[i]) != defaults.end();
        if (!exists && default_folder) {
            ImGui::SameLine();
            ImGui::TextColored(kMuted, "(doesn't exist yet)");
            // downloads go into the first folder (song_downloads.h), made as needed
            if (i == 0 && ImGui::IsItemHovered()) {
                ImGui::SetTooltip("band3 makes it when you download songs; you can also make "
                                  "it yourself and put songs in it.");
            }
        } else if (!exists) {
            ImGui::SameLine();
            ImGui::TextColored(kWarn, "(not found)");
        }
        ImGui::PopClipRect();
        // SameLine's offsets are the cell's, the cursor's the window's
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + text_width + ImGui::GetStyle().ItemSpacing.x);
        if (ImGui::Button("Remove")) removed = i;
        ImGui::PopID();
    }
    if (folders.empty()) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "No song folders");
    }
    if (removed) {
        folders.erase(folders.begin() + static_cast<std::ptrdiff_t>(*removed));
        Apply(s.cvar, join(folders));
    }

    // adding one: typed, or from the system's folder dialog
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ButtonWidth("Add") + ButtonWidth("Browse...") + style.ItemSpacing.x * 2;
    EditText("##add", "Add a folder: its path, or Browse", new_folder_,
             ImGui::GetContentRegionAvail().x - buttons);
    // what's typed so far: new_folder_ takes it only once the field is left
    const TextField& field = text_fields_[ImGui::GetID("##add")];
    const std::string typed = field.text.empty() ? new_folder_ : std::string(field.text.data());
    ImGui::SameLine();
    const bool blank = typed.find_first_not_of(" \t") == std::string::npos;
    ImGui::BeginDisabled(blank);
    if (ImGui::Button("Add")) {
        folders.push_back(typed);
        Apply(s.cvar, join(folders));
        new_folder_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        StartFolderPick(Str(s.cvar), rex::path_to_utf8(model_.Env().anchor));
    }
}

namespace {

constexpr const char* kOther = "Other...";

// the meters' scale: silence to full scale, in dB
constexpr float kMeterFloor = -60.0f;

// where an amplitude sits on a meter, 0 to 1
float MeterPosition(float amplitude) {
    return (MicLevel::ToDecibels(amplitude, kMeterFloor) - kMeterFloor) / -kMeterFloor;
}

std::string SizeLabel(int width, int height) {
    return std::to_string(width) + " x " + std::to_string(height);
}

// a resolution setting's size, as the SDK reads it (a preset or WxH)
std::optional<std::pair<int, int>> ResolutionSize(std::string_view value) {
    int32_t width = 0, height = 0;
    if (!rex::graphics::video_mode_util::TryParseResolutionPreset(value, width, height)) {
        return std::nullopt;
    }
    return std::pair<int, int>(width, height);
}

}

void LauncherDialog::RefreshDeviceLists() {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point now = Clock::now();
    // a list is kept for this long, also while its tab is away, so going back
    // and forth between tabs doesn't list again each time
    constexpr auto kEvery = std::chrono::seconds(2);
    auto due = [&](bool showing, Clock::time_point& next) {
        if (!showing || now < next) return false;
        next = now + kEvery;
        return true;
    };
    auto shown = [&](std::string_view cvar) {
        const Setting* s = model_.Find(cvar);
        return s && model_.Visible(*s);
    };
    const bool mics_showing = stage_ == Stage::kEditing && current_tab_ == Tab::kAudio &&
                              shown("usb_mic_devices");
    // let go when the slots stop showing (OnDraw's CloseMeters)
    if (mics_showing) HoldAudio(true);
    if (due(mics_showing, next_mics_)) {
        std::vector<std::string> mics = RecordingDeviceNames();
        if (mics != mics_ || !mics_listed_) {
            mics_ = std::move(mics);
            mic_list_generation_++;
        }
        mics_listed_ = true;
    }
    if (due(current_tab_ == Tab::kControllers && shown("midi_drums_device"), next_midi_)) {
        midi_ports_ = MidiInputPorts();
        midi_listed_ = true;
    }
    if (due(current_tab_ == Tab::kGraphics && (shown("monitor") || shown("resolution")),
            next_monitors_)) {
        monitors_ = ListMonitors(host_.native_window ? host_.native_window() : nullptr);
    }
}

bool LauncherDialog::DeviceCombo(std::string_view key, const std::string& none,
                                 const std::vector<DeviceEntry>& devices,
                                 std::optional<size_t> selected, std::string& value,
                                 const char* empty_list, float width) {
    const bool custom = custom_rows_.contains(key);
    const bool blank = value.empty();
    const bool found = !blank && selected && *selected < devices.size();
    const std::string missing = value + " (not connected)";
    std::string preview = custom ? kOther : blank ? none : found ? devices[*selected].label : missing;
    const ImGuiStyle& style = ImGui::GetStyle();
    const float combo_width = custom ? std::min(width * 0.4f, Px(170)) : width;
    bool changed = false;
    auto pick = [&](const std::string& next) {
        custom_rows_.erase(std::string(key));
        if (next == value) return;
        value = next;
        changed = true;
    };
    ImGui::SetNextItemWidth(combo_width);
    if (ImGui::BeginCombo("##device", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
        const bool none_selected = !custom && blank;
        if (ImGui::Selectable(none.c_str(), none_selected)) pick({});
        if (none_selected) ImGui::SetItemDefaultFocus();
        for (size_t i = 0; i < devices.size(); i++) {
            ImGui::PushID(static_cast<int>(i));
            const bool is_selected = !custom && found && *selected == i;
            if (ImGui::Selectable(devices[i].label.c_str(), is_selected)) pick(devices[i].value);
            if (is_selected) ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        if (devices.empty()) {
            ImGui::BeginDisabled();
            ImGui::Selectable(empty_list, false);
            ImGui::EndDisabled();
        }
        if (!custom && !blank && !found) {
            // stays the setting until another is picked
            ImGui::Selectable(missing.c_str(), true);
            ImGui::SetItemDefaultFocus();
        }
        if (ImGui::Selectable(kOther, custom)) custom_rows_.insert(std::string(key));
        if (custom) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
    if (custom) {
        ImGui::SameLine();
        std::string typed = value;
        if (EditText("##typed", HintFor(key.substr(0, key.find('#'))), typed,
                     width - combo_width - style.ItemSpacing.x) &&
            typed != value) {
            value = typed;
            changed = true;
        }
    }
    return changed;
}

void LauncherDialog::CloseMeters() {
    for (SlotMeter& slot : meters_) slot = {};
    HoldAudio(false);
    audio_hold_failed_ = false;
}

void LauncherDialog::HoldAudio(bool hold) {
    if (hold == audio_held_ || (hold && audio_hold_failed_)) return;
    if (!hold) {
        SDL_QuitSubSystem(SDL_INIT_AUDIO);
        audio_held_ = false;
        return;
    }
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        REXLOG_WARN("Launcher: SDL's audio didn't start ({})", SDL_GetError());
        audio_hold_failed_ = true;
        return;
    }
    audio_held_ = true;
}

void LauncherDialog::DrawMicMeter(int slot, const std::optional<std::string>& device,
                                  float width) {
    SlotMeter& m = meters_[static_cast<size_t>(slot)];
    // a meter that couldn't record tries again when the microphones change
    if (m.meter && (!device || *device != m.device ||
                    (m.generation != mic_list_generation_ && !m.meter->recording()))) {
        m = {};
    }
    if (!device) return;
    if (!m.meter) {
        m.meter = std::make_unique<MicMeter>(*device);
        m.device = *device;
        m.generation = mic_list_generation_;
    }

    const float frame = ImGui::GetFrameHeight();
    if (!m.meter->recording()) {
        FontScope font(kSmallSize);
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kBad, "Can't record");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.meter->error().c_str());
        return;
    }

    const MicLevel::Reading level = m.meter->Level();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float height = std::max(Px(8), std::round(frame * 0.32f));
    const ImVec2 min(at.x, at.y + std::round((frame - height) / 2));
    const ImVec2 max(at.x + width, min.y + height);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(min, max, ImGui::GetColorU32(kFrame), height / 2);
    // the RMS: green, warming as it nears full scale
    const float rms = MeterPosition(level.rms);
    if (rms > 0) {
        const float rms_db = MicLevel::ToDecibels(level.rms, kMeterFloor);
        const ImVec4 color = rms_db > -6 ? kBad : rms_db > -18 ? kWarn : kGood;
        draw->AddRectFilled(min, ImVec2(min.x + std::max(width * rms, height), max.y),
                            ImGui::GetColorU32(color), height / 2);
    }
    // a tick every 12 dB, over the bar
    for (int db = -48; db < 0; db += 12) {
        const float x = std::round(min.x + width * (db - kMeterFloor) / -kMeterFloor);
        draw->AddLine(ImVec2(x, min.y), ImVec2(x, max.y), ImGui::GetColorU32(kBackground),
                      std::max(1.0f, Px(1)));
    }
    // the held peak, red when it reaches full scale
    const float peak = MeterPosition(level.peak);
    if (peak > 0) {
        const float x = min.x + std::clamp(width * peak, Px(1), width - Px(1));
        const ImVec4 color = MicLevel::ToDecibels(level.peak, kMeterFloor) > -1 ? kBad : kText;
        draw->AddLine(ImVec2(x, min.y - Px(3)), ImVec2(x, max.y + Px(3)),
                      ImGui::GetColorU32(color), std::max(2.0f, Px(2)));
    }
    ImGui::Dummy(ImVec2(width, frame));
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("%s\nPeak %.0f dB, RMS %.0f dB", m.meter->device_name().c_str(),
                          MicLevel::ToDecibels(level.peak, kMeterFloor),
                          MicLevel::ToDecibels(level.rms, kMeterFloor));
    }
}

void LauncherDialog::DrawMicSlots(const Setting& s) {
    static_assert(std::tuple_size_v<decltype(meters_)> == audio::usb_mic::kSlots);
    meters_drawn_ = true;
    std::vector<std::string> slots = audio::usb_mic::ParseDeviceList(model_.Value(s.cvar));
    slots.resize(audio::usb_mic::kSlots);
    // with every slot empty, slot 1 records the system's default microphone;
    // once another is named, an empty slot is just unused (usb_mic_capture.cpp)
    const bool all_empty = std::ranges::all_of(slots, [](const auto& v) { return v.empty(); });

    std::vector<DeviceEntry> devices;
    for (const auto& name : mics_) devices.push_back({name, MicSlotValue(name)});

    const ImGuiStyle& style = ImGui::GetStyle();
    const float label = ImGui::CalcTextSize("Mic 4").x + style.ItemSpacing.x * 2;
    for (int i = 0; i < audio::usb_mic::kSlots; i++) {
        ImGui::PushID(i);
        std::string& slot = slots[static_cast<size_t>(i)];
        const float start = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "Mic %d", i + 1);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + label);

        const bool default_slot = i == 0 && all_empty;
        const std::optional<size_t> selected = FindSavedDevice(mics_, slot);
        // the meter takes what's left after a comfortable dropdown, within limits
        const float avail = ImGui::GetContentRegionAvail().x;
        const float meter = std::clamp(avail - Px(380) - style.ItemSpacing.x, Px(120), Px(220));
        const float combo = std::max(avail - meter - style.ItemSpacing.x, Px(160));
        const std::string key = Str(s.cvar) + "#" + std::to_string(i);
        std::string value = slot;
        if (DeviceCombo(key, default_slot ? "System default" : "None", devices, selected, value,
                        "No microphones found", combo)) {
            slot = value;
            Apply(s.cvar, JoinMicSlots(slots));
        }

        std::optional<std::string> meter_device;
        if (default_slot && !mics_.empty()) {
            meter_device = "";
        } else if (!slot.empty() && selected) {
            meter_device = slot;
        }
        ImGui::SameLine();
        DrawMicMeter(i, meter_device, meter);
        if (!meter_device) ImGui::NewLine();
        ImGui::PopID();
    }
    if (mics_listed_ && mics_.empty()) {
        FontScope font(kSmallSize);
        ImGui::TextColored(kWarn, "No microphones found. Connect one, and it shows up here.");
    }
}

void LauncherDialog::DrawMidiPort(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    std::vector<std::string> ports;
    std::vector<DeviceEntry> devices;
    for (const auto& port : midi_ports_) {
        ports.push_back(port.port);
        devices.push_back({port.name, port.name});
    }
    // the driver opens the first port when the setting is empty: say which
    std::string none = "First MIDI port";
    if (const auto first = FindMidiPort(ports, "")) none += " (" + devices[*first].label + ")";
    const std::optional<size_t> selected =
        value.empty() ? std::nullopt : FindMidiPort(ports, value);
    // a dropdown's width, and the row's when it has a text field too
    const float width = custom_rows_.contains(s.cvar) ? ImGui::GetContentRegionAvail().x
                                                      : ControlWidth();
    if (DeviceCombo(s.cvar, none, devices, selected, value, "No MIDI ports found", width)) {
        Apply(s.cvar, value);
    }
    if (midi_listed_ && midi_ports_.empty()) {
        FontScope font(kSmallSize);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kWarn, "No MIDI ports found. Connect the kit: band3 opens it when it "
                                  "appears.");
        ImGui::PopTextWrapPos();
    }
}

void LauncherDialog::DrawMonitor(const Setting& s) {
    // numbered where the monitors can't be listed
    if (monitors_.empty()) {
        DrawCombo(s);
        return;
    }
    const int current = static_cast<int>(AsInt(model_.Value(s.cvar)).value_or(0));
    auto label = [&](int monitor) {
        if (monitor <= 0) return std::string("Default (where the window is)");
        const size_t i = static_cast<size_t>(monitor - 1);
        if (i >= monitors_.size()) return "Monitor " + std::to_string(monitor) + " (not connected)";
        const Monitor& m = monitors_[i];
        std::string text = std::to_string(monitor) + ": " +
                           (m.name.empty() ? "Monitor " + std::to_string(monitor) : m.name);
        if (m.primary) text += " (primary)";
        return text;
    };
    const std::string preview = label(current);
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        const int count = static_cast<int>(monitors_.size());
        for (int monitor = 0; monitor <= count; monitor++) {
            const bool is_selected = monitor == current;
            if (ImGui::Selectable(label(monitor).c_str(), is_selected)) {
                Apply(s.cvar, std::to_string(monitor));
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        }
        if (current < 0 || current > count) {
            ImGui::Selectable(preview.c_str(), true);
            ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

void LauncherDialog::DrawResolution(const Setting& s) {
    // the presets alone where the monitors can't be listed
    if (monitors_.empty()) {
        DrawComboText(s);
        return;
    }
    // the chosen monitor's modes; the default's is the one the window is on
    const int chosen = static_cast<int>(AsInt(model_.Value("monitor")).value_or(0));
    const Monitor* monitor = nullptr;
    if (chosen >= 1 && static_cast<size_t>(chosen) <= monitors_.size()) {
        monitor = &monitors_[static_cast<size_t>(chosen - 1)];
    } else {
        for (const auto& m : monitors_) {
            if (m.has_window || (!monitor && m.primary)) monitor = &m;
        }
        if (!monitor) monitor = &monitors_.front();
    }

    struct Entry {
        std::string label;
        std::string value;
        int width = 0, height = 0;
    };
    // the monitor's sizes, once each (the setting has no refresh rate), and
    // then the presets it doesn't have
    std::vector<Entry> modes;
    for (const DisplayMode& mode : monitor->modes) {
        const bool listed = std::ranges::any_of(modes, [&](const Entry& e) {
            return e.width == mode.width && e.height == mode.height;
        });
        if (listed) continue;
        const bool desktop =
            mode.width == monitor->current.width && mode.height == monitor->current.height;
        modes.push_back({SizeLabel(mode.width, mode.height) + (desktop ? " (desktop)" : ""),
                         std::to_string(mode.width) + "x" + std::to_string(mode.height),
                         mode.width, mode.height});
    }
    std::vector<Entry> presets;
    for (const Choice& choice : s.choices) {
        const auto size = ResolutionSize(choice.value);
        if (!size) continue;
        const bool listed = std::ranges::any_of(modes, [&](const Entry& e) {
            return e.width == size->first && e.height == size->second;
        });
        if (!listed) presets.push_back({Str(choice.label), Str(choice.value), size->first, size->second});
    }

    // the value selects the entry of its size, whichever way it's written
    const std::string value = model_.Value(s.cvar);
    const auto size = ResolutionSize(value);
    const Entry* selected = nullptr;
    for (const auto* list : {&modes, &presets}) {
        for (const Entry& e : *list) {
            if (size && e.width == size->first && e.height == size->second) selected = &e;
        }
    }
    // a size this monitor doesn't list (set for another) is still shown as one
    std::optional<Entry> other;
    if (size && !selected) other = Entry{SizeLabel(size->first, size->second), value};
    const bool custom = custom_rows_.contains(s.cvar) || (!value.empty() && !size);
    const std::string default_label = s.choices.empty() ? "Default" : Str(s.choices.front().label);
    const std::string preview = custom     ? kOther
                                : selected ? selected->label
                                : other    ? other->label
                                           : default_label;

    const float avail = ImGui::GetContentRegionAvail().x;
    const float combo_width = custom ? std::min(avail * 0.45f, Px(240)) : ControlWidth();
    ImGui::SetNextItemWidth(combo_width);
    if (ImGui::BeginCombo("##choice", preview.c_str())) {
        auto entry = [&](const std::string& label, const std::string& next, bool is_selected) {
            if (ImGui::Selectable(label.c_str(), is_selected)) {
                custom_rows_.erase(Str(s.cvar));
                Apply(s.cvar, next);
            }
            if (is_selected) ImGui::SetItemDefaultFocus();
        };
        entry(default_label, "", !custom && value.empty());
        for (const Entry& e : modes) entry(e.label, e.value, !custom && selected == &e);
        if (other) entry(other->label, other->value, !custom);
        if (!presets.empty()) {
            ImGui::Separator();
            for (const Entry& e : presets) entry(e.label, e.value, !custom && selected == &e);
        }
        if (ImGui::Selectable(kOther, custom)) custom_rows_.insert(Str(s.cvar));
        if (custom) ImGui::SetItemDefaultFocus();
        ImGui::EndCombo();
    }
    if (!custom) return;
    ImGui::SameLine();
    std::string typed = value;
    if (EditText("##custom", HintFor(s.cvar), typed, ImGui::GetContentRegionAvail().x)) {
        Apply(s.cvar, typed);
    }
}

void LauncherDialog::DrawJoypadLag(const Setting& s) {
    const ImGuiStyle& style = ImGui::GetStyle();
    float label = 0;
    for (const auto& type : LagEditorTypes()) {
        label = std::max(label, ImGui::CalcTextSize(Str(type.label).c_str()).x);
    }
    label += ImGui::GetFrameHeight() + style.ItemInnerSpacing.x + style.ItemSpacing.x * 2;
    for (const auto& type : LagEditorTypes()) {
        ImGui::PushID(static_cast<int>(type.type));
        const std::string text = model_.Value(s.cvar);
        const std::optional<float> lag = LagFor(text, type.type);
        bool on = lag.has_value();
        const float start = ImGui::GetCursorPosX();
        if (ImGui::Checkbox(Str(type.label).c_str(), &on)) {
            Apply(s.cvar, WithLag(text, type.type, on ? std::optional<float>(0.0f)
                                                     : std::nullopt));
        }
        if (ImGui::IsItemHovered()) NoteHovered(s.cvar);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + label);
        if (lag) {
            float ms = *lag;
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, Px(190)));
            if (ImGui::InputFloat("##ms", &ms, 1.0f, 10.0f, "%.0f ms")) {
                Apply(s.cvar, WithLag(model_.Value(s.cvar), type.type, std::round(ms)));
            }
        } else {
            ImGui::AlignTextToFramePadding();
            ImGui::TextColored(kMuted, "The game's own");
        }
        ImGui::PopID();
    }
}

void LauncherDialog::DrawWindowMode(const Setting& s) {
    (void)s;
    static constexpr std::pair<WindowMode, const char*> kModes[] = {
        {WindowMode::kWindowed, "Windowed"},
        {WindowMode::kBorderless, "Borderless fullscreen"},
        {WindowMode::kExclusive, "Exclusive fullscreen"},
    };
    const WindowMode mode = model_.GetWindowMode();
    const char* preview = "";
    for (const auto& [m, name] : kModes) {
        if (m == mode) preview = name;
    }
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("##value", preview)) {
        for (const auto& [m, name] : kModes) {
            if (ImGui::Selectable(name, m == mode)) model_.SetWindowMode(m);
            if (m == mode) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
}

bool LauncherDialog::EditText(const char* id, const char* hint, std::string& value, float width) {
    TextField& field = text_fields_[ImGui::GetID(id)];
    // the setting's value until the field is being edited
    if (!field.editing || field.text.empty()) {
        field.text.assign(value.begin(), value.end());
        field.text.resize(std::max<size_t>(value.size() + 1, 256), '\0');
    }
    ImGui::SetNextItemWidth(std::max(width, Px(80)));
    ImGui::InputTextWithHint(id, hint, field.text.data(), field.text.size(),
                             ImGuiInputTextFlags_CallbackResize, ResizeText, &field.text);
    field.editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        value = field.text.data();
        return true;
    }
    return false;
}

void LauncherDialog::NoteHovered(std::string_view cvar) { hovered_next_ = cvar; }

bool LauncherDialog::Apply(std::string_view cvar, std::string_view value) {
    if (model_.Set(cvar, value)) {
        if (const auto it = row_problems_.find(cvar); it != row_problems_.end()) {
            row_problems_.erase(it);
        }
        return true;
    }
    REXLOG_WARN("Launcher: {} didn't accept \"{}\"", cvar, value);
    row_problems_[Str(cvar)] = model_.Refusal(cvar, value);
    return false;
}

void LauncherDialog::StartFolderPick(std::string target, const std::string& from) {
    // a dialog that never answered (gamescope may not show it) doesn't block
    // another: its answer goes to the pick it was for, which is dropped
    pick_ = std::make_shared<FolderPick>();
    pick_target_ = std::move(target);
    auto* holder = new std::shared_ptr<FolderPick>(pick_);
    SDL_ShowOpenFolderDialog(OnFolderPicked, holder, nullptr, from.empty() ? nullptr : from.c_str(),
                             false);
}

void LauncherDialog::TakeFolderPick() {
    if (!pick_) return;
    std::optional<std::string> folder;
    {
        std::lock_guard lock(pick_->mutex);
        if (!pick_->done) return;
        folder = pick_->folder;
        if (!pick_->error.empty()) {
            REXLOG_WARN("Launcher: the folder dialog failed: {}", pick_->error);
            row_problems_[pick_target_] = "The folder dialog didn't open; type the folder instead.";
        }
    }
    pick_.reset();
    if (!folder) return;
    if (pick_target_ == "content_folders") {
        std::string list = model_.Value(pick_target_);
        if (!paths::SplitList(list).empty()) list += '|';
        Apply(pick_target_, list + *folder);
    } else {
        Apply(pick_target_, *folder);
    }
}

void LauncherDialog::DrawFooter() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::BeginChild("##footer", ImVec2(0, 0), ImGuiChildFlags_NavFlattened,
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        ImGui::EndChild();
        ImGui::PopStyleVar();
        return;
    }
    const ImVec2 top = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(top, ImVec2(top.x + ImGui::GetContentRegionAvail().x, top.y),
                                        ImGui::GetColorU32(kLine), std::max(1.0f, Px(1)));
    ImGui::Dummy(ImVec2(0, Px(8)));

    // the hovered setting's name and description, three lines at most
    {
        FontScope font(kSmallSize);
        if (ImGui::BeginChild("##description", ImVec2(0, DescriptionHeight()), ImGuiChildFlags_None,
                              ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                  ImGuiWindowFlags_NoNav)) {
            ImGui::PushTextWrapPos(0);
            const Setting* setting = hovered_.empty() ? nullptr : model_.Find(hovered_);
            if (setting) {
                const std::string_view text = model_.Description(hovered_);
                const std::string label = Str(setting->label);
                ImGui::TextColored(kAccent, "%s", label.c_str());
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "(%s)", Str(setting->cvar).c_str());
                if (!text.empty()) ImGui::TextUnformatted(text.data(), text.data() + text.size());
            } else {
                ImGui::TextColored(kMuted,
                                   "Point at a setting to see what it does. Changes apply as you "
                                   "make them; Save keeps them for next time.");
                if (!device_panel_.NavPads().empty()) {
                    ImGui::TextColored(kMuted, "With a controller: LB and RB switch tabs, Start "
                                               "plays.%s",
                                       current_tab_ == Tab::kControllers
                                           ? " A on a device tests it; Back stops."
                                           : "");
                }
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }

    // the bottom row: the startup box, what saving did, and the buttons
    const Setting* show = model_.Find("show_launcher");
    const bool show_locked = show && model_.IsLocked(show->cvar);
    ImGui::BeginDisabled(show_locked);
    bool show_at_startup = model_.ShowAtStartup();
    if (ImGui::Checkbox("Show this screen at startup", &show_at_startup)) {
        model_.SetShowAtStartup(show_at_startup);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) NoteHovered("show_launcher");

    const float play_padding = Px(28);
    const float buttons = ButtonWidth("Save") + ButtonWidth("Close") + ButtonWidth("Play") +
                          play_padding * 2 + style.ItemSpacing.x * 2;
    const float right = ImGui::GetWindowContentRegionMax().x;
    ImGui::SameLine();
    const float status_start = ImGui::GetCursorPosX() + Px(12);
    const float status_end = right - buttons - Px(12);
    std::string status = save_message_;
    ImVec4 status_color = save_failed_ ? kBad : kGood;
    if (!save_failed_ && HasUnsavedChanges()) {
        status = "Unsaved changes";
        status_color = kMuted;
    }
    if (!status.empty() && status_end > status_start) {
        ImGui::SetCursorPosX(status_start);
        ImGui::AlignTextToFramePadding();
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushClipRect(at, ImVec2(at.x + (status_end - status_start),
                                       at.y + ImGui::GetFrameHeight()),
                            true);
        ImGui::TextColored(status_color, "%s", status.c_str());
        ImGui::PopClipRect();
        if (ImGui::IsItemHovered() && ImGui::CalcTextSize(status.c_str()).x > status_end - status_start) {
            ImGui::SetTooltip("%s", status.c_str());
        }
        ImGui::SameLine();
    }

    ImGui::SetCursorPosX(right - buttons);
    if (ImGui::Button("Save")) Save();
    ImGui::SameLine();
    if (ImGui::Button("Close")) RequestQuit();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
    ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 1, 1, 1));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                        ImVec2(style.FramePadding.x + play_padding, style.FramePadding.y));
    if (ImGui::Button("Play")) Play();
    // Enter or Space plays once the keyboard has moved to it; Play has it first
    ImGui::SetItemDefaultFocus();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor(4);

    ImGui::EndChild();
    ImGui::PopStyleVar();
}

void LauncherDialog::DrawPrompts() {
    if (open_quit_prompt_) {
        ImGui::OpenPopup(kQuitPrompt);
        open_quit_prompt_ = false;
    }
    if (open_play_prompt_) {
        ImGui::OpenPopup(kPlayPrompt);
        open_play_prompt_ = false;
    }
    if (open_save_failed_prompt_) {
        ImGui::OpenPopup(kSaveFailedPrompt);
        open_save_failed_prompt_ = false;
    }
    const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(Px(22), Px(18)));
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kQuitPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("Your changes haven't been saved.");
        ImGui::Spacing();
        if (ImGui::Button("Save and quit")) {
            ImGui::CloseCurrentPopup();
            if (Save() && host_.quit) host_.quit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Quit without saving")) {
            ImGui::CloseCurrentPopup();
            if (host_.quit) host_.quit();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kPlayPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        ImGui::TextUnformatted("The game data folder doesn't look like Rock Band 3's:");
        ImGui::TextColored(kWarn, "%s", DescribeProblem(check_.problem));
        ImGui::Spacing();
        if (ImGui::Button("Play")) {
            ImGui::CloseCurrentPopup();
            Start();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    // a band3.toml that can't be written (a read-only install) mustn't keep
    // the game from starting: the settings are already applied to the cvars
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal(kSaveFailedPrompt, nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        // the system's messages end in a full stop of their own ("Access is denied.")
        std::string_view error = save_error_;
        while (!error.empty() && (error.back() == '.' || error.back() == ' ')) {
            error.remove_suffix(1);
        }
        ImGui::PushTextWrapPos(Px(620));
        ImGui::TextColored(kBad, "Couldn't save your settings (%.*s).",
                           static_cast<int>(error.size()), error.data());
        ImGui::PopTextWrapPos();
        ImGui::TextUnformatted("Play anyway with these settings for this session?");
        // a restart reads band3.toml, so Play doesn't restart without it
        if (input::InputBackendChanged()) {
            ImGui::TextColored(kWarn, "The new input backend applies only once it's saved.");
        }
        ImGui::Spacing();
        if (ImGui::Button("Play anyway")) {
            ImGui::CloseCurrentPopup();
            Begin();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::SetItemDefaultFocus();
        ImGui::EndPopup();
    }
    ImGui::PopStyleVar();
}

void LauncherDialog::DrawStarting(ImGuiIO& io) {
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoNav |
                                   ImGuiWindowFlags_NoBringToFrontOnFocus;
    if (ImGui::Begin("band3##launcher_starting", nullptr, flags)) {
        FontScope font(kTitleSize);
        const char* text = restart_ ? "Restarting band3..." : "Starting Rock Band 3...";
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
}

void LauncherDialog::HandleNav(const NavEdges& edges) {
    if (edges.tab_previous || edges.tab_next) {
        const int count = static_cast<int>(std::size(kTabs));
        int i = 0;
        while (i < count && kTabs[i].first != current_tab_) i++;
        i = (i + (edges.tab_next ? 1 : count - 1)) % count;
        pending_tab_ = kTabs[i].first;
    }
    // not while a prompt or a dropdown is open
    if (edges.start && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) Play();
}

void LauncherDialog::Play() {
    // the folder may have changed since the last check
    RefreshGameDataCheck(true);
    if (!check_.ok) {
        open_play_prompt_ = true;
        return;
    }
    Start();
}

void LauncherDialog::Start() {
    if (stage_ != Stage::kEditing) return;
    if (!Save()) {
        open_save_failed_prompt_ = true;
        return;
    }
    Begin();
}

void LauncherDialog::Begin() {
    if (stage_ != Stage::kEditing) return;
    restart_ = RestartsForInput({
        .backend_changed = input::InputBackendChanged(),
        .saved = !save_failed_,
        .test_port = REXCVAR_GET(test_port) != 0,
    });
    REXLOG_INFO("Launcher: Play{}{}", save_failed_ ? ", without saving" : "",
                restart_ ? ", restarting band3 for the new input backend" : "");
    stage_ = Stage::kStarting;
    starting_frames_ = 0;
}

bool LauncherDialog::Save() {
    const std::string name = rex::path_to_utf8(host_.config_path.filename());
    const SaveResult result = model_.Save(host_.config_path);
    save_failed_ = !result.ok;
    if (!result.ok) {
        save_error_ = result.error;
        save_message_ = "Couldn't save " + name + ": " + result.error;
        REXLOG_ERROR("Launcher: couldn't save {}: {}", rex::path_to_utf8(host_.config_path),
                     result.error);
        return false;
    }
    const std::string backup = rex::path_to_utf8(BackupPath(host_.config_path).filename());
    if (result.had_comments) {
        save_message_ = "Saved; your old file with its comments is in " + backup + ".";
    } else if (result.backed_up) {
        save_message_ =
            "Saved. The old " + name + " couldn't be read and is kept as " + backup + ".";
    } else {
        save_message_ = "Saved to " + name + ".";
    }
    config_problem_.reset();
    REXLOG_INFO("Launcher: saved {}{}", rex::path_to_utf8(host_.config_path),
                result.backed_up ? " (the old file is kept as .bak)" : "");
    return true;
}

}
