#include "settings_page.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <rex/filesystem.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/logging.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <string>
#include <utility>
#include "src/Audio/usb_mic.h"
#include "src/Input/input_system.h"
#include "src/paths.h"
#include "src/Render/renderer_mode.h"
#include "gamepad_nav.h"
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

std::string Str(std::string_view s) { return std::string(s); }

// small text under a row's control, wrapped to the column
void RowNote(const ImVec4& color, const char* text) {
    FontScope font(kSmallSize);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::PushTextWrapPos(0);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// the renderer row's line about the value chosen
const char* RendererLine(std::string_view value) {
    switch (render::ParseRenderer(value).value_or(render::RendererMode::kEmulated)) {
    case render::RendererMode::kNative:
        return "band3's own renderer alone, at the window's size; no emulated Xbox 360 GPU";
    case render::RendererMode::kEmulated: return "The emulated Xbox 360 GPU alone";
    case render::RendererMode::kBoth:
        return "Both run, the native picture shown; F8 switches to the emulated GPU's and back. "
               "For comparing them: it costs the GPU both";
    }
    return "";
}

std::string FormatNumber(double v) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%g", v);
    return buffer;
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
    if (cvar == "native_max_height") return "Lines tall, e.g. 900";
    if (cvar == "forced_venue") return "A venue or a comma separated list";
    return "";
}

}

SettingsPage::SettingsPage(SettingsModel& model, Where where, PageHost host)
    : model_(model), where_(where), features_(FeaturesFor(where)), host_(std::move(host)) {}

SettingsPage::~SettingsPage() { CloseMeters(); }

void SettingsPage::BeginFrame() {
    hovered_ = std::move(hovered_next_);
    hovered_next_.clear();
    meters_drawn_ = false;
#ifndef _WIN32
    // the folder dialog goes through the xdg desktop portal off Windows, which
    // may answer only while SDL's events are pumped, and nothing pumps them
    // before the game runs. Unverified on Linux; harmless without video.
    if (pick_ && where_ == Where::kLauncher) SDL_PumpEvents();
#endif
    TakeFolderPick();
    RefreshGameDataCheck(false);
}

void SettingsPage::EndFrame() {
    // the meters record only while their slots show: not on another tab, nor
    // with usb_mics off
    if (!meters_drawn_) CloseMeters();
}

const GameDataCheck& SettingsPage::GameData(bool now) {
    RefreshGameDataCheck(now);
    return check_;
}

void SettingsPage::DrawDeckBanner() {
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

void SettingsPage::DrawSections(Tab tab) {
    for (const std::string_view section : SectionsOf(model_.Table(), tab)) {
        DrawSection(tab, section);
    }
}

void SettingsPage::DrawSection(Tab tab, std::string_view section) {
    std::vector<const Setting*> rows;
    for (const auto& s : model_.Table()) {
        if (s.tab == tab && s.section == section && model_.Visible(s)) rows.push_back(&s);
    }

    const std::optional<std::string> note = model_.SectionNote(tab, section);
    if (rows.empty() && !note) return;

    const std::string name(section);
    SectionHeading(name.c_str());
    if (note) {
        FontScope font(kSmallSize);
        ImGui::PushTextWrapPos(0);
        ImGui::TextColored(kMuted, "%s", note->c_str());
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, Px(4)));
    }

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

void SettingsPage::DrawRow(const Setting& setting) {
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
    const bool read_only = model_.ReadOnly(setting.cvar);
    ImGui::TableSetColumnIndex(1);
    ImGui::BeginGroup();
    ImGui::BeginDisabled(locked || read_only);
    DrawControl(setting);
    ImGui::EndDisabled();
    if (locked) {
        const Lock lock = model_.IsLocked(setting.cvar) ? model_.LockOf(setting.cvar)
                                                        : model_.LockOf(setting.companion);
        FontScope font(kSmallSize);
        ImGui::TextColored(kMuted, "%s; it can't be changed here.", LockReason(lock));
    } else if (read_only) {
        RowNote(kMuted, "Set as band3 starts only (the command line or band3.toml)");
    }
    DrawRowNotes(setting);
    if (const auto warning = model_.Warning(setting.cvar)) RowNote(kWarn, warning->c_str());
    if (const auto problem = row_problems_.find(setting.cvar); problem != row_problems_.end()) {
        RowNote(kBad, problem->second.c_str());
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) ||
        NavCursorWithin(ImGui::GetItemRectMin(), ImGui::GetItemRectMax())) {
        NoteHovered(setting.cvar);
    }

    ImGui::TableSetColumnIndex(2);
    if (!locked && !read_only && model_.IsChanged(setting.cvar)) {
        if (ImGui::Button("Reset") && model_.Reset(setting.cvar)) {
            row_problems_.erase(Str(setting.cvar));
        }
        if (ImGui::IsItemHovered()) NoteHovered(setting.cvar);
    }
    ImGui::PopID();
}

void SettingsPage::DrawRowNotes(const Setting& setting) {
    // what the renderer chosen does
    if (setting.cvar == "renderer") RowNote(kMuted, RendererLine(model_.Value(setting.cvar)));
    if (where_ == Where::kInGame) {
        // the game has started with what it reads once
        if (model_.WaitsForNextStart(setting.cvar)) RowNote(kWarn, "Applies at the next start");
        return;
    }
    // On the launcher: the input system can't be swapped while band3 runs,
    // nor the graphics system, chosen before the launcher showed, so Play
    // restarts band3 for them
    const bool restarts = (setting.cvar == "input_backend" && input::InputBackendChanged()) ||
                          (setting.cvar == "renderer" && model_.RendererNeedsRestart());
    if (restarts) RowNote(kMuted, "Applies when you press Play (band3 restarts)");
}

void SettingsPage::DrawControl(const Setting& setting) {
    switch (setting.widget) {
    case Widget::kCheckbox: DrawCheckbox(setting); break;
    case Widget::kCombo: DrawCombo(setting); break;
    case Widget::kComboText: DrawComboText(setting); break;
    case Widget::kIntStepper: DrawIntStepper(setting); break;
    case Widget::kIntSlider: DrawIntSlider(setting); break;
    case Widget::kFloatSlider: DrawFloatSlider(setting, false); break;
    case Widget::kFloatInput: DrawFloatInput(setting); break;
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

void SettingsPage::DrawCheckbox(const Setting& s) {
    bool on = AsBool(model_.Value(s.cvar));
    if (ImGui::Checkbox("##value", &on)) Apply(s.cvar, on ? "true" : "false");
}

void SettingsPage::DrawCombo(const Setting& s) {
    const int index = model_.ChoiceIndex(s);
    const std::string custom = model_.Value(s.cvar) + " (set elsewhere)";
    const std::string preview = index >= 0 ? Str(s.choices[index].label) : custom;
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::BeginCombo("##value", preview.c_str())) {
        for (size_t i = 0; i < s.choices.size(); i++) {
            const bool selected = static_cast<int>(i) == index;
            // a choice this platform doesn't offer shows only while it's the value
            if (!selected && !model_.Offered(s.choices[i])) continue;
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

void SettingsPage::DrawComboText(const Setting& s) {
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

void SettingsPage::DrawIntStepper(const Setting& s) {
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

void SettingsPage::DrawIntSlider(const Setting& s) {
    int v = static_cast<int>(AsInt(model_.Value(s.cvar)).value_or(0));
    const Range range = s.range.value_or(Range{0, 100, 1});
    ImGui::SetNextItemWidth(ControlWidth());
    if (ImGui::SliderInt("##value", &v, static_cast<int>(range.min), static_cast<int>(range.max))) {
        Apply(s.cvar, std::to_string(v));
    }
}

void SettingsPage::DrawFloatSlider(const Setting& s, bool percent) {
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

void SettingsPage::DrawFloatInput(const Setting& s) {
    double v = AsFloat(model_.Value(s.cvar)).value_or(0.0);
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, Px(210)));
    if (ImGui::InputDouble("##value", &v, 0.0, 0.0, "%g",
                           ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (s.range && s.range->max > s.range->min) {
            v = std::clamp(v, s.range->min, s.range->max);
        }
        Apply(s.cvar, FormatNumber(v));
    }
}

void SettingsPage::DrawText(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    if (EditText("##value", HintFor(s.cvar), value, ImGui::GetContentRegionAvail().x)) {
        Apply(s.cvar, value);
    }
}

void SettingsPage::DrawPath(const Setting& s) {
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

void SettingsPage::RefreshGameDataCheck(bool now) {
    using Clock = std::chrono::steady_clock;
    const std::filesystem::path root =
        host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (!now && check_done_ && root == checked_root_ && Clock::now() < next_check_) return;
    checked_root_ = root;
    check_ = CheckGameData(root);
    check_done_ = true;
    next_check_ = Clock::now() + std::chrono::seconds(1);
}

void SettingsPage::DrawGameDataCheck() {
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

bool SettingsPage::FolderExists(const std::filesystem::path& folder) {
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

void SettingsPage::DrawFolderList(const Setting& s) {
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

void SettingsPage::RefreshDeviceLists(Tab tab) {
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
    const bool mics_showing = tab == Tab::kAudio && shown("usb_mic_devices");
    // let go when the slots stop showing (EndFrame's CloseMeters)
    if (mics_showing) HoldAudio(true);
    if (due(mics_showing, next_mics_)) {
        std::vector<std::string> mics = RecordingDeviceNames();
        if (mics != mics_ || !mics_listed_) {
            mics_ = std::move(mics);
            mic_list_generation_++;
        }
        mics_listed_ = true;
    }
    if (due(tab == Tab::kControllers && shown("midi_drums_device"), next_midi_)) {
        midi_ports_ = MidiInputPorts();
        midi_listed_ = true;
    }
    if (due(tab == Tab::kGraphics && (shown("monitor") || shown("resolution")),
            next_monitors_)) {
        monitors_ = ListMonitors(host_.native_window ? host_.native_window() : nullptr);
        monitors_generation_++;
    }
}

bool SettingsPage::DeviceCombo(std::string_view key, const std::string& none,
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

void SettingsPage::CloseMeters() {
    for (SlotMeter& slot : meters_) slot = {};
    HoldAudio(false);
    audio_hold_failed_ = false;
}

void SettingsPage::HoldAudio(bool hold) {
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

void SettingsPage::DrawMicMeter(int slot, const std::optional<std::string>& device,
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

void SettingsPage::DrawMicSlots(const Setting& s) {
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
        // the meter takes what's left after a comfortable dropdown, within
        // limits; without meters (in game) the dropdown is as wide as others
        const float avail = ImGui::GetContentRegionAvail().x;
        const bool meters = features_.mic_meters;
        const float meter =
            meters ? std::clamp(avail - Px(380) - style.ItemSpacing.x, Px(120), Px(220)) : 0.0f;
        const float combo = meters ? std::max(avail - meter - style.ItemSpacing.x, Px(160))
                                   : std::min(avail, Px(380));
        const std::string key = Str(s.cvar) + "#" + std::to_string(i);
        std::string value = slot;
        if (DeviceCombo(key, default_slot ? "System default" : "None", devices, selected, value,
                        "No microphones found", combo)) {
            slot = value;
            Apply(s.cvar, JoinMicSlots(slots));
        }

        // a meter records from the microphone, so none in game, where the
        // game's capture has it
        if (meters) {
            std::optional<std::string> meter_device;
            if (default_slot && !mics_.empty()) {
                meter_device = "";
            } else if (!slot.empty() && selected) {
                meter_device = slot;
            }
            ImGui::SameLine();
            DrawMicMeter(i, meter_device, meter);
            if (!meter_device) ImGui::NewLine();
        }
        ImGui::PopID();
    }
    if (mics_listed_ && mics_.empty()) {
        FontScope font(kSmallSize);
        ImGui::TextColored(kWarn, "No microphones found. Connect one, and it shows up here.");
    }
}

void SettingsPage::DrawMidiPort(const Setting& s) {
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

void SettingsPage::DrawMonitor(const Setting& s) {
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

void SettingsPage::DrawResolution(const Setting& s) {
    // the presets alone where the monitors can't be listed
    if (monitors_.empty()) {
        DrawComboText(s);
        return;
    }
    // the chosen monitor's modes; the default's is the one the window is on
    const int chosen = static_cast<int>(AsInt(model_.Value("monitor")).value_or(0));
    if (!resolution_lists_ || resolution_lists_->monitors_generation != monitors_generation_ ||
        resolution_lists_->monitor != chosen) {
        const Monitor* monitor = nullptr;
        if (chosen >= 1 && static_cast<size_t>(chosen) <= monitors_.size()) {
            monitor = &monitors_[static_cast<size_t>(chosen - 1)];
        } else {
            for (const auto& m : monitors_) {
                if (m.has_window || (!monitor && m.primary)) monitor = &m;
            }
            if (!monitor) monitor = &monitors_.front();
        }

        // the monitor's sizes, once each (the setting has no refresh rate), and
        // then the presets it doesn't have
        ResolutionLists lists;
        lists.monitors_generation = monitors_generation_;
        lists.monitor = chosen;
        for (const DisplayMode& mode : monitor->modes) {
            const bool listed = std::ranges::any_of(lists.modes, [&](const ResolutionEntry& e) {
                return e.width == mode.width && e.height == mode.height;
            });
            if (listed) continue;
            const bool desktop =
                mode.width == monitor->current.width && mode.height == monitor->current.height;
            lists.modes.push_back(
                {SizeLabel(mode.width, mode.height) + (desktop ? " (desktop)" : ""),
                 std::to_string(mode.width) + "x" + std::to_string(mode.height), mode.width,
                 mode.height});
        }
        for (const Choice& choice : s.choices) {
            const auto size = ResolutionSize(choice.value);
            if (!size) continue;
            const bool listed = std::ranges::any_of(lists.modes, [&](const ResolutionEntry& e) {
                return e.width == size->first && e.height == size->second;
            });
            if (!listed) {
                lists.presets.push_back(
                    {Str(choice.label), Str(choice.value), size->first, size->second});
            }
        }
        resolution_lists_ = std::move(lists);
    }
    using Entry = ResolutionEntry;
    const std::vector<Entry>& modes = resolution_lists_->modes;
    const std::vector<Entry>& presets = resolution_lists_->presets;

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

void SettingsPage::DrawJoypadLag(const Setting& s) {
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

void SettingsPage::DrawWindowMode(const Setting& s) {
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

bool SettingsPage::EditText(const char* id, const char* hint, std::string& value, float width) {
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

bool SettingsPage::Apply(std::string_view cvar, std::string_view value) {
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

void SettingsPage::StartFolderPick(std::string target, const std::string& from) {
    // a dialog that never answered (gamescope may not show it) doesn't block
    // another: its answer goes to the pick it was for, which is dropped
    pick_ = std::make_shared<FolderPick>();
    pick_target_ = std::move(target);
    auto* holder = new std::shared_ptr<FolderPick>(pick_);
    SDL_ShowOpenFolderDialog(OnFolderPicked, holder, nullptr, from.empty() ? nullptr : from.c_str(),
                             false);
}

void SettingsPage::TakeFolderPick() {
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

void SettingsPage::NoteHovered(std::string_view cvar) { hovered_next_ = cvar; }

float SettingsPage::DescriptionHeight() { return Px(kSmallSize) * 4 + Px(2); }

void SettingsPage::DrawDescription(const std::function<void()>& idle) {
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
            // a generated row's label is its name already
            if (setting->label != setting->cvar) {
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "(%s)", Str(setting->cvar).c_str());
            }
            if (model_.NextStartOnly(setting->cvar)) {
                ImGui::SameLine();
                ImGui::TextColored(kMuted, "- applies at the next start");
            }
            if (!text.empty()) ImGui::TextUnformatted(text.data(), text.data() + text.size());
        } else if (idle) {
            idle();
        }
        ImGui::PopTextWrapPos();
    }
    ImGui::EndChild();
}

void SettingsPage::DrawStartupBox() {
    const Setting* show = model_.Find("show_launcher");
    const bool show_locked = show && model_.IsLocked(show->cvar);
    ImGui::BeginDisabled(show_locked);
    bool show_at_startup = model_.ShowAtStartup();
    const char* label = where_ == Where::kLauncher ? "Show this screen at startup"
                                                    : "Show the launcher at startup";
    if (ImGui::Checkbox(label, &show_at_startup)) {
        model_.SetShowAtStartup(show_at_startup);
    }
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) NoteHovered("show_launcher");
}

}
