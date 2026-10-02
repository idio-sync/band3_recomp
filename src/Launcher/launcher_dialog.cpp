#include "launcher_dialog.h"
#include <SDL3/SDL_dialog.h>
#include <SDL3/SDL_error.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_video.h>
#include <imgui.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include "src/Audio/usb_mic.h"
#include "src/paths.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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

// Colours

const ImVec4 kBackground(0.071f, 0.075f, 0.090f, 1.0f);
const ImVec4 kPopup(0.105f, 0.110f, 0.135f, 1.0f);
const ImVec4 kFrame(0.150f, 0.157f, 0.190f, 1.0f);
const ImVec4 kFrameHover(0.200f, 0.208f, 0.250f, 1.0f);
const ImVec4 kFrameActive(0.245f, 0.255f, 0.305f, 1.0f);
const ImVec4 kLine(0.200f, 0.208f, 0.245f, 1.0f);
const ImVec4 kText(0.925f, 0.925f, 0.945f, 1.0f);
const ImVec4 kMuted(0.585f, 0.600f, 0.660f, 1.0f);
const ImVec4 kAccent(0.937f, 0.435f, 0.235f, 1.0f);
const ImVec4 kAccentHover(1.000f, 0.540f, 0.330f, 1.0f);
const ImVec4 kAccentActive(0.820f, 0.355f, 0.180f, 1.0f);
const ImVec4 kAccentDim(0.380f, 0.200f, 0.130f, 1.0f);
const ImVec4 kGood(0.470f, 0.840f, 0.500f, 1.0f);
const ImVec4 kWarn(1.000f, 0.700f, 0.330f, 1.0f);
const ImVec4 kBad(1.000f, 0.470f, 0.420f, 1.0f);
const ImVec4 kDeckBanner(0.105f, 0.150f, 0.215f, 1.0f);
const ImVec4 kProblemBanner(0.235f, 0.125f, 0.090f, 1.0f);

// Fonts and scale

// the sizes the launcher's font is baked at: the SDK's ImGui renderer can't
// bake new sizes on the fly, so each size is drawn from the nearest bake
// at or above it
constexpr float kFontBakes[] = {17, 20, 23, 30, 40, 60};
std::array<ImFont*, std::size(kFontBakes)> g_fonts{};

// text sizes at a 1280x720 window
constexpr float kBodySize = 20;
constexpr float kSmallSize = 17;
constexpr float kHeadingSize = 23;
constexpr float kTitleSize = 30;

// the page's scale, set at the start of each frame
float g_scale = 1.0f;

float Px(float v) { return v * g_scale; }

ImFont* FontFor(float size) {
    ImFont* best = nullptr;
    for (size_t i = 0; i < g_fonts.size(); i++) {
        if (!g_fonts[i]) continue;
        best = g_fonts[i];
        if (kFontBakes[i] >= size - 0.5f) break;
    }
    return best;
}

// the launcher's font at a 720p size, scaled with the page
class FontScope {
public:
    explicit FontScope(float size) {
        const float px = Px(size);
        ImGui::PushFont(FontFor(px), px);
    }
    ~FontScope() { ImGui::PopFont(); }
    FontScope(const FontScope&) = delete;
    FontScope& operator=(const FontScope&) = delete;
};

// a readable system font, if there's one where it's expected
std::filesystem::path FindUiFont() {
    std::vector<std::filesystem::path> candidates;
#ifdef _WIN32
    wchar_t windows[MAX_PATH] = {};
    const UINT length = GetWindowsDirectoryW(windows, MAX_PATH);
    if (length > 0 && length < MAX_PATH) {
        const std::filesystem::path fonts = std::filesystem::path(windows) / "Fonts";
        // Segoe UI on Windows; Proton (a Steam Deck) has Arial's metric twin
        for (const char* name : {"segoeui.ttf", "arial.ttf", "tahoma.ttf"}) {
            candidates.push_back(fonts / name);
        }
    }
#else
    for (const char* file : {"/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"}) {
        candidates.emplace_back(file);
    }
#endif
    for (const auto& file : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(file, ec)) return file;
    }
    return {};
}

std::vector<char> ReadFile(const std::filesystem::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return {};
    return std::vector<char>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

// the launcher's look, over the SDK's style for the length of its draw
void ApplyStyle(ImGuiStyle& style, float s) {
    style.FontScaleMain = 1.0f;
    style.FontScaleDpi = 1.0f;
    style.WindowPadding = ImVec2(28 * s, 18 * s);
    style.FramePadding = ImVec2(10 * s, 6 * s);
    style.ItemSpacing = ImVec2(12 * s, 9 * s);
    style.ItemInnerSpacing = ImVec2(8 * s, 6 * s);
    style.CellPadding = ImVec2(6 * s, 5 * s);
    style.IndentSpacing = 20 * s;
    style.ScrollbarSize = 14 * s;
    style.GrabMinSize = 16 * s;
    style.WindowRounding = 0;
    style.ChildRounding = 6 * s;
    style.FrameRounding = 5 * s;
    style.PopupRounding = 6 * s;
    style.ScrollbarRounding = 7 * s;
    style.GrabRounding = 4 * s;
    style.TabRounding = 5 * s;
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.PopupBorderSize = 1;
    style.FrameBorderSize = 0;
    style.TabBorderSize = 0;
    style.TabBarBorderSize = 2 * s;
    style.TabBarOverlineSize = 2 * s;
    style.SeparatorTextBorderSize = 1;
    style.DisabledAlpha = 0.45f;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = kMuted;
    c[ImGuiCol_WindowBg] = kBackground;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPopup;
    c[ImGuiCol_Border] = kLine;
    c[ImGuiCol_BorderShadow] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = kAccentDim;
    c[ImGuiCol_HeaderHovered] = kFrameHover;
    c[ImGuiCol_HeaderActive] = kFrameActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Separator] = kLine;
    c[ImGuiCol_Tab] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = kFrame;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TabDimmedSelected] = kFrame;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
    c[ImGuiCol_ScrollbarGrabActive] = kFrameActive;
    c[ImGuiCol_TextSelectedBg] = ImVec4(kAccent.x, kAccent.y, kAccent.z, 0.35f);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    c[ImGuiCol_TitleBg] = kFrame;
    c[ImGuiCol_TitleBgActive] = kAccentDim;
    c[ImGuiCol_TitleBgCollapsed] = kFrame;
}

// how much bigger than at 1280x720 the page is drawn
float ScaleFor(const ImVec2& display) {
    const float s = std::min(display.x / 1280.0f, display.y / 720.0f);
    return std::clamp(s, 0.75f, 3.0f);
}

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
    if (cvar == "midi_drums_device") return "The first MIDI port";
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
    // keyboard until it closes
    ImGuiIO& io = GetIO();
    saved_config_flags_ = io.ConfigFlags;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    config_problem_ = ConfigFileProblem(host_.config_path);
    if (config_problem_) {
        REXLOG_WARN("Launcher: {} couldn't be read: {}", rex::path_to_utf8(host_.config_path),
                    *config_problem_);
    }
}

LauncherDialog::~LauncherDialog() {
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
    g_scale = scale_ = ScaleFor(io.DisplaySize);
    ApplyStyle(style, scale_);
    {
        FontScope font(kBodySize);
        if (stage_ == Stage::kEditing) {
            TakeFolderPick();
            DrawPage(io);
        } else {
            DrawStarting(io);
        }
    }
    style = sdk_style;

    // the frame just drawn shows "Starting" while the runtime is built, which
    // holds up the UI thread for a while; the next one starts the game
    if (stage_ == Stage::kStarting && ++starting_frames_ > 1) {
        stage_ = Stage::kStarted;
        if (host_.start_game) host_.start_game();
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
                if (ImGui::BeginTabItem(name)) {
                    current_tab_ = tab;
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
        }
        ImGui::PopStyleVar();
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
        if (ImGui::BeginChild("##settings", ImVec2(0, settings_height), ImGuiChildFlags_None)) {
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
    {
        FontScope font(kHeadingSize);
        ImGui::TextColored(kAccent, "%s", name.c_str());
    }
    // a hairline under the heading
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(
        ImVec2(at.x, at.y - Px(4)),
        ImVec2(at.x + ImGui::GetContentRegionAvail().x, at.y - Px(4)),
        ImGui::GetColorU32(kLine), std::max(1.0f, Px(1)));
    ImGui::Dummy(ImVec2(0, Px(2)));

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
    if (const auto warning = model_.Warning(setting.cvar)) {
        FontScope font(kSmallSize);
        ImGui::PushStyleColor(ImGuiCol_Text, kWarn);
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted(warning->c_str());
        ImGui::PopTextWrapPos();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) NoteHovered(setting.cvar);

    ImGui::TableSetColumnIndex(2);
    if (!locked && model_.IsChanged(setting.cvar)) {
        if (ImGui::Button("Reset")) model_.Reset(setting.cvar);
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
    if (ImGui::Checkbox("##value", &on)) model_.Set(s.cvar, on ? "true" : "false");
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
                model_.Set(s.cvar, s.choices[i].value);
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
                model_.Set(s.cvar, s.choices[i].value);
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
        model_.Set(s.cvar, value);
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
        model_.Set(s.cvar, std::to_string(v));
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
        model_.Set(s.cvar, std::to_string(v));
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
        model_.Set(s.cvar, FormatNumber(std::clamp(next, range.min, range.max)));
    }
}

void LauncherDialog::DrawText(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    if (EditText("##value", HintFor(s.cvar), value, ImGui::GetContentRegionAvail().x)) {
        model_.Set(s.cvar, value);
    }
}

void LauncherDialog::DrawPath(const Setting& s) {
    std::string value = model_.Value(s.cvar);
    const std::string fallback =
        rex::path_to_utf8(std::filesystem::path(model_.EffectiveDefault(s.cvar)).make_preferred());
    const std::string hint = fallback.empty() ? std::string() : "Default: " + fallback;
    const float browse = ButtonWidth("Browse...");
    const float field = ImGui::GetContentRegionAvail().x - browse - ImGui::GetStyle().ItemSpacing.x;
    if (EditText("##value", hint.c_str(), value, field)) model_.Set(s.cvar, value);
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

void LauncherDialog::DrawGameDataCheck() {
    const std::filesystem::path root =
        host_.game_data_root ? host_.game_data_root() : std::filesystem::path();
    if (!check_done_ || root != checked_root_) {
        checked_root_ = root;
        check_ = CheckGameData(root);
        check_done_ = true;
    }
    FontScope font(kSmallSize);
    const std::string shown = root.empty() ? "(not set)" : rex::path_to_utf8(root);
    ImGui::PushTextWrapPos(0);
    if (check_.ok) {
        ImGui::TextColored(kGood, "Found Rock Band 3 in %s", shown.c_str());
    } else {
        ImGui::TextColored(kWarn, "%s (%s)", DescribeProblem(check_.problem), shown.c_str());
    }
    ImGui::PopTextWrapPos();
}

void LauncherDialog::DrawFolderList(const Setting& s) {
    const std::string value = model_.Value(s.cvar);
    std::vector<std::string> folders = paths::SplitList(value);
    auto join = [](const std::vector<std::string>& list) {
        std::string out;
        for (const auto& folder : list) {
            if (!out.empty()) out += '|';
            out += folder;
        }
        return out;
    };

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
        std::error_code ec;
        const bool exists = std::filesystem::is_directory(resolved, ec);
        ImGui::AlignTextToFramePadding();
        const float text_width = ImGui::GetContentRegionAvail().x - remove -
                                 ImGui::GetStyle().ItemSpacing.x;
        const float start = ImGui::GetCursorPosX();
        ImGui::PushClipRect(ImGui::GetCursorScreenPos(),
                            ImVec2(ImGui::GetCursorScreenPos().x + text_width,
                                   ImGui::GetCursorScreenPos().y + ImGui::GetFrameHeight()),
                            true);
        ImGui::TextUnformatted(rex::path_to_utf8(resolved).c_str());
        if (!exists) {
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
        model_.Set(s.cvar, join(folders));
    }

    // adding one: typed, or from the system's folder dialog
    const ImGuiStyle& style = ImGui::GetStyle();
    const float buttons = ButtonWidth("Add") + ButtonWidth("Browse...") + style.ItemSpacing.x * 2;
    EditText("##add", "Add a folder: its path, or Browse", new_folder_,
             ImGui::GetContentRegionAvail().x - buttons);
    ImGui::SameLine();
    const bool blank = new_folder_.find_first_not_of(" \t") == std::string::npos;
    ImGui::BeginDisabled(blank);
    if (ImGui::Button("Add")) {
        folders.push_back(new_folder_);
        model_.Set(s.cvar, join(folders));
        new_folder_.clear();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Browse...")) {
        StartFolderPick(Str(s.cvar), rex::path_to_utf8(model_.Env().anchor));
    }
}

void LauncherDialog::DrawMicSlots(const Setting& s) {
    std::vector<std::string> slots = audio::usb_mic::ParseDeviceList(model_.Value(s.cvar));
    slots.resize(audio::usb_mic::kSlots);
    const float label = ImGui::CalcTextSize("Mic 4").x + ImGui::GetStyle().ItemSpacing.x * 2;
    for (int i = 0; i < audio::usb_mic::kSlots; i++) {
        ImGui::PushID(i);
        const float start = ImGui::GetCursorPosX();
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(kMuted, "Mic %d", i + 1);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + label);
        const char* hint = i == 0 ? "The system's default recording device" : "Not used";
        if (EditText("##slot", hint, slots[static_cast<size_t>(i)],
                     ImGui::GetContentRegionAvail().x)) {
            model_.Set(s.cvar, JoinMicSlots(slots));
        }
        ImGui::PopID();
    }
    FontScope font(kSmallSize);
    ImGui::TextColored(kMuted, "A name, or part of one; device lists come in a later version.");
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
            model_.Set(s.cvar, WithLag(text, type.type, on ? std::optional<float>(0.0f)
                                                         : std::nullopt));
        }
        if (ImGui::IsItemHovered()) NoteHovered(s.cvar);
        ImGui::SameLine();
        ImGui::SetCursorPosX(start + label);
        if (lag) {
            float ms = *lag;
            ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, Px(190)));
            if (ImGui::InputFloat("##ms", &ms, 1.0f, 10.0f, "%.0f ms")) {
                model_.Set(s.cvar, WithLag(model_.Value(s.cvar), type.type, std::round(ms)));
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
            save_failed_ = true;
            save_message_ = "The folder dialog didn't open; type the folder instead.";
        }
    }
    pick_.reset();
    if (!folder) return;
    if (pick_target_ == "content_folders") {
        std::string list = model_.Value(pick_target_);
        if (!paths::SplitList(list).empty()) list += '|';
        model_.Set(pick_target_, list + *folder);
    } else {
        model_.Set(pick_target_, *folder);
    }
}

void LauncherDialog::DrawFooter() {
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    if (!ImGui::BeginChild("##footer", ImVec2(0, 0), ImGuiChildFlags_None,
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
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();
    }

    // the bottom row: the startup box, what saving did, and the buttons
    const Setting* show = model_.Find("show_launcher");
    const bool show_locked = show && model_.IsLocked(show->cvar);
    ImGui::BeginDisabled(show_locked);
    ImGui::Checkbox("Show this screen at startup", &show_at_startup_);
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
        const char* text = "Starting Rock Band 3...";
        const ImVec2 size = ImGui::CalcTextSize(text);
        ImGui::SetCursorPos(ImVec2((io.DisplaySize.x - size.x) / 2, (io.DisplaySize.y - size.y) / 2));
        ImGui::TextUnformatted(text);
    }
    ImGui::End();
}

void LauncherDialog::Play() {
    if (!check_.ok) {
        open_play_prompt_ = true;
        return;
    }
    Start();
}

bool LauncherDialog::Start() {
    if (stage_ != Stage::kEditing) return false;
    // into the cvar as well as the file, so F4's "Save to config" keeps it
    model_.Set("show_launcher", show_at_startup_ ? "true" : "false");
    if (!Save()) return false;
    REXLOG_INFO("Launcher: Play");
    stage_ = Stage::kStarting;
    starting_frames_ = 0;
    return true;
}

bool LauncherDialog::Save() {
    const std::string name = rex::path_to_utf8(host_.config_path.filename());
    const SaveResult result = model_.Save(host_.config_path);
    save_failed_ = !result.ok;
    if (!result.ok) {
        save_message_ = "Couldn't save " + name + ": " + result.error;
        REXLOG_ERROR("Launcher: couldn't save {}: {}", rex::path_to_utf8(host_.config_path),
                     result.error);
        return false;
    }
    if (result.backed_up) {
        save_message_ = "Saved. The old " + name + " couldn't be read and is kept as " +
                        rex::path_to_utf8(BackupPath(host_.config_path).filename()) + ".";
    } else {
        save_message_ = "Saved to " + name + ".";
    }
    config_problem_.reset();
    REXLOG_INFO("Launcher: saved {}{}", rex::path_to_utf8(host_.config_path),
                result.backed_up ? " (the old file is kept as .bak)" : "");
    return true;
}

void AddLauncherFonts(ImFontAtlas* atlas) {
    // Latin, Latin-1 and Latin Extended-A, and punctuation (dashes, quotes, ...)
    static const ImWchar kRanges[] = {0x0020, 0x00FF, 0x0100, 0x017F, 0x2010, 0x205E, 0};
    const std::filesystem::path file = FindUiFont();
    std::vector<char> data = file.empty() ? std::vector<char>() : ReadFile(file);

    // one copy of the font's data, owned by the first font, for every size
    void* shared = nullptr;
    int shared_size = 0;
    bool owned = false;
    if (!data.empty()) {
        shared_size = static_cast<int>(data.size());
        shared = IM_ALLOC(data.size());
        std::memcpy(shared, data.data(), data.size());
        owned = true;
    } else if (atlas->Sources.Size > 0 && atlas->Sources[0].FontData) {
        // the SDK's own font, larger
        shared = atlas->Sources[0].FontData;
        shared_size = atlas->Sources[0].FontDataSize;
    }
    if (!shared) {
        REXLOG_WARN("Launcher: no font to draw with; it uses the SDK's small one");
        return;
    }
    for (size_t i = 0; i < std::size(kFontBakes); i++) {
        ImFontConfig config;
        config.FontDataOwnedByAtlas = owned && i == 0;
        std::snprintf(config.Name, sizeof(config.Name), "band3 launcher %.0fpx", kFontBakes[i]);
        g_fonts[i] = atlas->AddFontFromMemoryTTF(shared, shared_size, kFontBakes[i], &config,
                                                 kRanges);
        if (!g_fonts[i] && i == 0 && owned) IM_FREE(shared);
        if (!g_fonts[i]) break;
    }
    REXLOG_INFO("Launcher: font {}",
                data.empty() ? std::string("the SDK's own") : rex::path_to_utf8(file));
}

bool ShiftHeld() {
#ifdef _WIN32
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
#else
    return false;
#endif
}

double DisplayRefreshRate(void* native_window) {
#ifdef _WIN32
    HMONITOR monitor =
        MonitorFromWindow(static_cast<HWND>(native_window), MONITOR_DEFAULTTOPRIMARY);
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!monitor || !GetMonitorInfoW(monitor, &info)) return 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (!EnumDisplaySettingsW(info.szDevice, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    // 0 and 1 stand for the hardware's default rate
    return mode.dmDisplayFrequency > 1 ? mode.dmDisplayFrequency : 0;
#else
    (void)native_window;
    // band3's SDL copy only has video while the native view runs; the SDK's
    // copy owns the window, so this is the primary display's rate at best
    if (!SDL_WasInit(SDL_INIT_VIDEO)) return 0;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetPrimaryDisplay());
    return mode && mode->refresh_rate > 0 ? mode->refresh_rate : 0;
#endif
}

}
