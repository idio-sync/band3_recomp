// band3 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/settings_overlay.h>
#include <imgui.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>

#include "build_tag.h"
#include "config.h"
#include "crash_trace.h"
#include "game_writes.h"
#include "paths.h"
#include "relaunch.h"
#include "settings.h"
#include "stall_watch.h"
#include "steam_deck.h"
#include "Audio/usb_mic_capture.h"
#include "Content/content_hooks.h"
#include "Content/live_content.h"
#include "Game/SongCache.h"
#include "Hooks/frame_pacing.h"
#include "Input/input_system.h"
#include "Input/instrument_lab.h"
#include "Input/menu_shortcut_dialog.h"
#include "Input/virtual_instrument.h"
#include "Launcher/game_data_check.h"
#include "Launcher/ingame_settings_dialog.h"
#include "Launcher/launcher_cvars.h"
#include "Launcher/launcher_dialog.h"
#include "Launcher/launcher_platform.h"
#include "Launcher/launcher_start.h"
#include "Launcher/launcher_style.h"
#include "Launcher/mic_meter.h"
#include "Net/discord.h"
#include "Net/liveless_rooms_panel.h"
#include "Net/online_hooks.h"
#include "Net/http_server.h"
#include "Render/gpu_view.h"
#include "Render/native_view.h"
#include "Render/renderer_switch.h"
#include "Render/scene_capture.h"
#include "Render/sync_gpu/native_only.h"
#include "Render/sync_gpu/sync_graphics_system.h"
#include "Test/test_server.h"

// always attached, and draws nothing while debug_overlay is off, so the
// setting can be flipped in F4
class DebugOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit DebugOverlayDialog(rex::ui::ImGuiDrawer* imgui_drawer)
      : rex::ui::ImGuiDialog(imgui_drawer) {}

  // hidden while the launcher is up
  void set_hidden(bool hidden) { hidden_ = hidden; }

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (hidden_ || !REXCVAR_GET(debug_overlay)) return;
    UpdateGameRate();
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(240, 76), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
      // the game's frames (what the frame cap paces, and hit timing follows),
      // and the window's paints, which the display's refresh holds back
      ImGui::Text("Game   %.1f FPS (%.2f ms)", game_fps_,
                  game_fps_ > 0 ? 1000.0 / game_fps_ : 0.0);
      ImGui::Text("Window %.1f FPS", io.Framerate);
    }
    ImGui::End();
  }

 private:
  // the game's Presents a second, over the last half second or more
  void UpdateGameRate() {
    const auto now = std::chrono::steady_clock::now();
    const uint64_t frames = band3::render::GamePresentCount();
    const double seconds = std::chrono::duration<double>(now - rate_since_).count();
    if (seconds < 0.5) return;
    game_fps_ = (frames - rate_frames_) / seconds;
    rate_frames_ = frames;
    rate_since_ = now;
  }

  bool hidden_ = false;
  double game_fps_ = 0.0;
  uint64_t rate_frames_ = 0;
  std::chrono::steady_clock::time_point rate_since_ = std::chrono::steady_clock::now();
};

class Band3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  std::unique_ptr<DebugOverlayDialog> debug_overlay_;
  std::unique_ptr<band3::input::InstrumentLabDialog> instrument_lab_;
  std::unique_ptr<band3::input::MenuShortcutDialog> menu_shortcut_;
  // the Liveless Rooms panel, see src/Net/liveless_rooms_panel.h
  std::unique_ptr<band3::rooms::RoomsPanelDialog> rooms_panel_;
  // the native view, see src/Render/native_view.h
  std::unique_ptr<band3::render::NativeViewDialog> native_view_;
  // the launcher while it's up, before the game starts (src/Launcher/)
  std::unique_ptr<band3::launcher::LauncherDialog> launcher_;
  // band3's settings in game, what F4 opens (src/Launcher/ingame_settings_dialog.h)
  std::unique_ptr<band3::launcher::InGameSettingsDialog> ingame_settings_;
  // the SDK's own settings menu, while its "All settings..." has it open
  std::unique_ptr<rex::ui::SettingsDialog> all_settings_;
  // OnFinalizePaths' folders and resume, kept for the launcher's Play, and
  // what the path rule needs from the ini
  rex::PathConfig path_defaults_;
  std::function<void(rex::PathConfig)> resume_;
  std::filesystem::path anchor_;
  bool cache_in_ini_ = false;
  // quitting from the launcher was confirmed, so the window may close
  bool quit_confirmed_ = false;
  // --launcher, once read (LauncherFlag)
  std::optional<bool> launcher_flag_;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Band3App>(new Band3App(ctx, "band3",
        PPCImageConfig));
  }

  // paths are fixed before band3.toml loads, so band3_config.ini's are the ones
  // read here, relative to its folder; the command line wins over the ini, and
  // OnFinalizePaths puts band3.toml's (and the launcher's) over the ini's
  void OnConfigurePaths(rex::PathConfig& paths) override {
    const auto anchor = band3::IniAnchor();
    auto from_ini = [&](const char* cvar, const std::string& value, std::filesystem::path& out) {
      if (rex::cvar::GetFlagSource(cvar) != rex::cvar::Source::kDefault || value.empty()) return false;
      out = band3::paths::Resolve(value, anchor);
      return true;
    };
    from_ini("game_data_root", band3::ReadIniGameDataRoot(), paths.game_data_root);
    const bool user_set = from_ini("user_data_root", band3::ReadIniString("user_data_root"),
                                   paths.user_data_root);
    const bool cache_set = from_ini("cache_root", band3::ReadIniString("cache_root"), paths.cache_root);
    // the SDK put the cache in the default user data folder; keep it with the new one
    if (user_set && !cache_set && rex::cvar::GetFlagSource("cache_root") == rex::cvar::Source::kDefault) {
      paths.cache_root = paths.user_data_root / "cache";
    }
    band3::SetGameDataRoot(paths.game_data_root);
  }

  // band3.toml, the environment and the command line are applied by now, and
  // the window and input system don't exist yet, so everything set here applies
  // at startup
  void OnPostInitLogging() override {
    // band3's first line, so any log says which build wrote it
    REXLOG_INFO("band3 build {}", band3::BuildTag());
    // --settings_reference=<file>: write it and quit, before anything else
    // starts (no window, no game, nothing for a crash notice to report)
    if (const std::string& out = REXCVAR_GET(settings_reference); !out.empty()) {
      std::quick_exit(band3::launcher::WriteSettingsReference(rex::to_path(out)) ? 0 : 1);
    }
    // a relaunch (rb3e_relaunch_game) starts before the last run has closed
    band3::relaunch::WaitForPrevious();
    LogFolders(game_data_root(), user_data_root(), cache_root());
    // before the ini, so a desktop ini's window settings don't undo them
    band3::steam_deck::ApplyDefaults();
    band3::ApplyLegacyIni();

    // band3's defaults for SDK settings (a shorter audio queue, a window)
    // unless told otherwise
    for (const auto& d : band3::settings::kStartupDefaults) {
      if (rex::cvar::GetFlagSource(d.cvar) == rex::cvar::Source::kDefault) {
        rex::cvar::SetFlagByName(d.cvar, d.value);
        rex::cvar::ClearPendingRestartFlags();
      }
    }

#ifndef _WIN32
    if (rex::cvar::GetFlagByName("input_backend") == "xinput") {
      REXLOG_WARN("input_backend = xinput is Windows-only, using sdl");
      rex::cvar::SetFlagByName("input_backend", "sdl");
    }
#endif

    // before OnPreSetup reads renderer
    band3::settings::MigrateRendererSettings();
    band3::AddSettingArgs();
    band3::settings::Init();
    band3::settings::SnapshotStartupSettings();
    band3::input::InitVirtualInstrument();
    band3::test::Init();
#ifdef _WIN32
    // GPU hang reports, before SetupPresentation makes the SDK's device
    if (REXCVAR_GET(dred)) band3::crash_trace::EnableDred();
#endif
    // the last run's crash, if it crashed: not for the test harness, which
    // reads its own runs' reports
    if (REXCVAR_GET(test_port) == 0) band3::crash_trace::ShowLastCrashNotice();
  }

  static void LogFolders(const std::filesystem::path& game_data,
                         const std::filesystem::path& user_data,
                         const std::filesystem::path& cache) {
    REXLOG_INFO("Folders: game data {}, user data {}, cache {} (ini: {})",
                rex::path_to_utf8(game_data), rex::path_to_utf8(user_data),
                rex::path_to_utf8(cache), rex::path_to_utf8(band3::LegacyIniPath()));
  }

  // The folders the game would start with: OnFinalizePaths' defaults with the
  // path rule (paths.h) applied for the folder settings as they are now, so
  // ones from band3.toml or the launcher count. The launcher asks every frame.
  rex::PathConfig RulePaths() const {
    using rex::cvar::Source;
    using band3::paths::PathSource;
    // fixed: the command line and the environment, which the launcher shows
    // locked for the same reason (launcher_cvars.cpp)
    auto setting = [](const char* cvar) {
      const Source source = rex::cvar::GetFlagSource(cvar);
      const PathSource from = source == Source::kDefault ? PathSource::kUnset
                              : source == Source::kCommandLine || source == Source::kEnvironment
                                  ? PathSource::kFixed
                                  : PathSource::kSaved;
      return band3::paths::PathSetting{rex::cvar::GetFlagByName(cvar), from};
    };
    band3::paths::PathRuleInputs in;
    in.defaults = {path_defaults_.game_data_root, path_defaults_.user_data_root,
                   path_defaults_.cache_root};
    in.game_data = setting("game_data_root");
    in.user_data = setting("user_data_root");
    in.cache = setting("cache_root");
    in.cache_in_ini = cache_in_ini_;
    in.anchor = anchor_;
    const band3::paths::Folders folders = band3::paths::ApplyPathRule(in);

    rex::PathConfig paths = path_defaults_;
    paths.game_data_root = folders.game_data;
    paths.user_data_root = folders.user_data;
    paths.cache_root = folders.cache;
    return paths;
  }

  // RulePaths, as the game starts with them
  rex::PathConfig FinalPaths() const {
    rex::PathConfig paths = RulePaths();
    band3::SetGameDataRoot(paths.game_data_root);
    LogFolders(paths.game_data_root, paths.user_data_root, paths.cache_root);
    return paths;
  }

  // The window and ImGui are up and the runtime isn't built yet: the launcher
  // shows here when it's wanted (launcher_start.h), holding the game back until
  // its Play calls resume.
  std::optional<rex::PathConfig> OnFinalizePaths(
      const rex::PathConfig& defaults, std::function<void(rex::PathConfig)> resume) override {
    path_defaults_ = defaults;
    anchor_ = band3::IniAnchor();
    cache_in_ini_ = !band3::ReadIniString("cache_root").empty();
    rex::PathConfig paths = FinalPaths();
    const auto check = band3::launcher::CheckGameData(paths.game_data_root);
    if (!check.ok) {
      REXLOG_WARN("Game data {}: {}", rex::path_to_utf8(paths.game_data_root),
                  band3::launcher::DescribeProblem(check.problem));
    }
    const auto decision = band3::launcher::DecideLauncher({
        .test_port = REXCVAR_GET(test_port) != 0,
        .relaunched = band3::relaunch::WasRelaunched(),
        .game_data_ok = check.ok,
        .launcher_flag = LauncherFlag(),
        .shift_held = band3::launcher::ShiftHeld(),
        .show_launcher = REXCVAR_GET(show_launcher),
    });
    // cleared, so a later "Save to config" doesn't keep it
    if (REXCVAR_GET(launcher) && !rex::cvar::SetFlagByName("launcher", "false")) {
      REXLOG_WARN("Launcher: couldn't clear the launcher setting");
    }
    REXLOG_INFO("Launcher: {}", decision.reason);
    if (!decision.show) {
      StartFrameCap();
      band3::settings::SnapshotStartupValues();
      return paths;
    }
    if (!imgui_drawer()) {
      REXLOG_WARN("Launcher: no ImGui to draw it with, starting the game");
      StartFrameCap();
      band3::settings::SnapshotStartupValues();
      return paths;
    }

    resume_ = std::move(resume);
    // the input system the game will take over (OnPreSetup), built now so the
    // launcher lists and tests the devices the game will read
    band3::input::PrepareInputSystem(window());
    launcher_ = std::make_unique<band3::launcher::LauncherDialog>(
        imgui_drawer(),
        band3::launcher::LauncherHost{
            .config_path = defaults.config_path,
            .path_defaults = {defaults.game_data_root, defaults.user_data_root},
            .anchor = anchor_,
            .game_data_root = [this] { return RulePaths().game_data_root; },
            .refresh_rate =
                [this] {
                  return band3::launcher::DisplayRefreshRate(
                      window() ? window()->GetNativeWindowHandle() : nullptr);
                },
            .native_window =
                [this]() -> void* { return window() ? window()->GetNativeWindowHandle() : nullptr; },
            .start_game = [this](bool restart) { StartFromLauncher(restart); },
            .quit = [this] { QuitFromLauncher(); },
        });
    if (debug_overlay_) debug_overlay_->set_hidden(true);
    return std::nullopt;
  }

  // --launcher, for this start only: a value saved in band3.toml (F4's "Save
  // to config" of a run started with it) doesn't count. Read once, since
  // OnFinalizePaths clears the setting once it has decided.
  bool LauncherFlag() {
    if (!launcher_flag_) {
      launcher_flag_ = REXCVAR_GET(launcher) &&
                       rex::cvar::GetFlagSource("launcher") != rex::cvar::Source::kConfig;
    }
    return *launcher_flag_;
  }

  // the launcher's Play, once its "Starting" frame is drawn: the settings read
  // at startup are taken again from what the launcher left, then the game
  // starts. With `restart` (a new input_backend, which the input system can't
  // switch to while band3 runs: input_system.h) band3 starts again instead,
  // with this run's command line less --launcher; a relaunch skips the
  // launcher and reads the band3.toml Play just saved.
  void StartFromLauncher(bool restart) {
    if (restart) {
      if (band3::relaunch::StartAgain()) {
        QuitFromLauncher();
        return;
      }
      REXLOG_WARN("Launcher: couldn't restart band3; the new input backend applies at the next start");
    }
    // what the launcher opened for itself goes; the input system stays open
    // for the game, which takes it over in Runtime::Setup (OnPreSetup)
    band3::launcher::CloseMicMeters();
    band3::input::ReadyInputForGame();
    band3::settings::SnapshotStartupSettings();
    band3::AddSettingArgs();
    // what the launcher changed is read from here on, so nothing waits on a
    // restart. That holds for swap_post_effect (FXAA) too: it's read once,
    // when the guest GPU is set up (GraphicsSystem::SetupGuestGpu, through
    // CommandProcessor::SetDesiredSwapPostEffect), which Runtime::Setup runs
    // inside resume, so the launcher's value applies without a relaunch; an F4
    // change to it in game applies at the next start.
    rex::cvar::ClearPendingRestartFlags();
    rex::PathConfig paths = FinalPaths();
    // not inside the launcher's draw: resume builds the runtime and starts the
    // native renderer on the same drawer
    app_context().CallInUIThreadDeferred([this, paths = std::move(paths)] {
      launcher_.reset();
      if (debug_overlay_) debug_overlay_->set_hidden(false);
      StartFrameCap();
      band3::settings::SnapshotStartupValues();
      auto resume = std::move(resume_);
      resume(paths);
    });
  }

  // The frame cap (src/Hooks/frame_pacing.h), with the settings the game
  // starts with. Not in OnPreSetup: that runs before the window, the launcher
  // and the GPU plugin (whose vsync it turns off) exist. Here they all do, and
  // the runtime that starts the plugin's vblank thread, which reads the
  // guest's refresh rate once, isn't built yet.
  void StartFrameCap() {
    band3::pacing::StartFrameCap(window() ? window()->GetNativeWindowHandle() : nullptr,
                                 [this](std::function<void()> task) {
                                   app_context().CallInUIThreadDeferred(std::move(task));
                                 });
  }

  void QuitFromLauncher() {
    quit_confirmed_ = true;
    app_context().CallInUIThreadDeferred([this] {
      if (window()) window()->RequestClose();
    });
  }

  // while the launcher's settings are being edited and aren't saved, closing
  // the window asks first; once Play is pressed it closes as usual
  // Closing the window ends the process without OnShutdown, so an accepted
  // close also deletes the port mapping and closes the Rooms connection.
  // The harness's quit closes without asking and stops online play itself
  // (src/Test/test_server.cpp).
  bool OnWindowCloseRequested() override {
    if (launcher_ && !quit_confirmed_ && launcher_->IsEditing() &&
        launcher_->HasUnsavedChanges()) {
      launcher_->RequestQuit();
      return false;
    }
    band3::online::Stop();
    return true;
  }

  // with renderer native the native renderer asks for the window's paints
  // itself, none while it's minimized, and one as soon as it's restored
  void OnWindowMinimized() override { band3::render::NativePresentMinimized(true); }
  void OnWindowRestored() override { band3::render::NativePresentMinimized(false); }

  // the launcher's larger font, which the in-game settings (F4) use too, so
  // every run has it
  void OnConfigureFonts(ImFontAtlas* atlas) override { band3::launcher::AddLauncherFonts(atlas); }

  // GPU emulation is a plugin (rexgpu-xenos) that the SDK leaves off unless
  // named; keep any gpu_plugin the user set in band3.toml. With renderer
  // native there's none: band3's sync-only GPU is the runtime's graphics
  // system instead (src/Render/sync_gpu/sync_graphics_system.h). Set here,
  // before the window exists, the SDK has it make its presenter and wires the
  // window and the overlays to it as it does the plugin's. A build with
  // nothing to present with but the plugin (neither Direct3D 12 nor Vulkan)
  // runs renderer native as emulated. This runs before the launcher shows
  // (OnFinalizePaths), so the launcher's Play restarts band3 for a renderer
  // that needs the other GPU (src/Render/renderer_mode.h).
  void OnPreSetup(rex::RuntimeConfig& config) override {
    namespace sync_gpu = band3::render::sync_gpu;
    const bool presentable = sync_gpu::CanPresentNativeOnly();
    const band3::render::StartupGpuPlan gpu =
        band3::render::PlanStartupGpu(REXCVAR_GET(renderer), config.gpu_plugin, presentable);
    sync_gpu::SetNativeOnly(gpu.native_only);
    band3::render::StartRendererSwitch(gpu, presentable);
    for (const std::string& line : gpu.log) REXLOG_INFO("{}", line);
    if (gpu.native_only) {
      config.gpu_plugin.clear();
      config.graphics = std::make_unique<sync_gpu::Band3GraphicsSystem>();
    } else if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // the SDK's input drivers plus band3's (virtual and PS3/Wii instruments):
    // the launcher's, when it was shown (OnFinalizePaths), or a new one. The
    // runtime calls Setup on it, which does nothing for an InputSystem (its
    // drivers were set up as they were added), then AttachWindow, which the
    // drivers take a second time without harm (input_system.h).
    config.input_factory = band3::input::CreateInputSystem;
  }

  // before the game starts, so its mic threads find the microphones running
  // and its first file opens find game:\ writable
  void OnPostSetup() override {
    // after the runtime's own exception handlers, which handle MMIO and GPU writes
    band3::crash_trace::WatchGuestFaults();
    band3::crash_trace::RunCrashTest();
    // a song cache rb3e_delete_songcache marked, before the game mounts it
    band3::song_cache::DeletePending(runtime()->user_data_root());
    band3::MountGameWrites(*runtime());
    // band3's content overrides hand saves to the SDK's own exports; without
    // them the first save would fail, so fail here instead
    if (!band3::content::ResolveSdkContentExports()) {
      REXLOG_ERROR("content: the SDK's content exports are missing, can't continue");
      std::abort();
    }
    // band3's sign-in overrides hand every call on to the SDK's
    if (!band3::online::ResolveSdkExports()) {
      REXLOG_ERROR("online: the SDK's user exports are missing, can't continue");
      std::abort();
    }
    band3::online::Start();
    band3::content::StartLiveContent(runtime()->file_system());
    band3::discord::Start();
    band3::audio::StartUsbMics();
    band3::render::StartDumpIfRequested();
    // the native renderer's drawer; here rather than in OnCreateDialogs,
    // which runs before the runtime has its graphics system
    if (auto* graphics = runtime()->graphics_system()) {
      band3::render::StartNativePresent(graphics->presenter(), graphics->provider(), window(),
                                        [this] { return immediate_drawer(); });
    }
    band3::test::StartServer(runtime(), &app_context(), window());
    band3::http::StartServer();
  }

  void OnShutdown() override {
    // before the ImGui drawer it's attached to goes
    launcher_.reset();
    band3::pacing::StopFrameCap();
    band3::stall_watch::Stop();
    // the Liveless Rooms client's thread reaches into the game (a join's
    // invite, a NAT punch from the game's socket), so it ends while the kernel
    // is still there, and before the test server that asks it for its status.
    // Closing the window doesn't come here: the SDK exits the process at once
    band3::online::Stop();
    band3::http::StopServer();
    band3::test::StopServer();
    rex::ui::UnregisterBind("bind_instrument_lab");
    rex::ui::UnregisterBind("bind_liveless_rooms");
    rex::ui::UnregisterBind("bind_native_view");
    rex::ui::UnregisterBind("bind_renderer");
    // before the input system goes: it holds the game's input while open
    all_settings_.reset();
    ingame_settings_.reset();
    // off the presenter, and the GPU done with the textures its paints read,
    // before the GPU device goes
    band3::render::StopNativePresent();
    band3::render::StopNativeView();
    // the F9 window's worker would draw on the CPU once the GPU is gone
    native_view_.reset();
    band3::render::GpuRenderer::Get().Shutdown();
    band3::discord::Stop();
    band3::audio::StopUsbMics();
  }

  // the SDK applies the fullscreen cvar to the window itself
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (drawer) {
      debug_overlay_ = std::make_unique<DebugOverlayDialog>(drawer);
      instrument_lab_ = std::make_unique<band3::input::InstrumentLabDialog>(drawer);
      // F6 and F9 do nothing while the launcher is up: both need the game
      rex::ui::RegisterBind("bind_instrument_lab", "F6", "Toggle the Instrument Lab", [this] {
        if (instrument_lab_ && !launcher_) instrument_lab_->Toggle();
      });
      native_view_ = std::make_unique<band3::render::NativeViewDialog>(
          drawer, [this] { return immediate_drawer(); });
      // F9: the SDK's achievements overlay has F7
      rex::ui::RegisterBind("bind_native_view", "F9", "Toggle the native view (experimental)", [this] {
        if (native_view_ && !launcher_) native_view_->Toggle();
      });
      // the picture, not the renderer setting (renderer_switch.h)
      rex::ui::RegisterBind("bind_renderer", "F8",
                            "Switch between the native and the emulated GPU's picture (renderer "
                            "both only)",
                            [] { band3::render::PressRendererSwitch(); });
      // also opened by the overshell's Invite Friends while Rooms is on, which
      // the panel watches for itself
      rooms_panel_ = std::make_unique<band3::rooms::RoomsPanelDialog>(drawer);
      rex::ui::RegisterBind("bind_liveless_rooms", "F10", "Toggle the Liveless Rooms panel", [this] {
        if (rooms_panel_ && !launcher_) rooms_panel_->Toggle();
      });
      // deferred: opening the settings menu adds a dialog, and this runs while
      // the dialogs draw
      menu_shortcut_ = std::make_unique<band3::input::MenuShortcutDialog>(
          drawer, [this](band3::input::MenuShortcutAction action) {
            const char* bind = action == band3::input::MenuShortcutAction::kSettings
                                   ? "bind_settings"
                                   : "bind_instrument_lab";
            app_context().CallInUIThreadDeferred([this, bind] { PressBind(bind); });
          });
      CreateSettings(drawer);
    } else {
      all_settings_.reset();
      ingame_settings_.reset();
      debug_overlay_.reset();
      instrument_lab_.reset();
      menu_shortcut_.reset();
      rooms_panel_.reset();
      native_view_.reset();
    }
  }

  // F4 (bind_settings) opens band3's own settings in game, in place of the
  // SDK's settings menu, which their "All settings..." opens. The SDK's
  // SetupOverlays registered the bind for its menu just before
  // OnCreateDialogs; it's registered again for band3's, keeping a key the
  // player bound it to. Like F6 and F9, it does nothing while the launcher
  // is up.
  void CreateSettings(rex::ui::ImGuiDrawer* drawer) {
    ingame_settings_ = std::make_unique<band3::launcher::InGameSettingsDialog>(
        drawer,
        band3::launcher::InGameHost{
            .config_path = [this] { return path_defaults_.config_path; },
            .path_defaults =
                [this] {
                  return band3::launcher::PathDefaults{path_defaults_.game_data_root,
                                                       path_defaults_.user_data_root};
                },
            .anchor = [this] { return anchor_; },
            .game_data_root = [this] { return game_data_root(); },
            .native_window =
                [this]() -> void* { return window() ? window()->GetNativeWindowHandle() : nullptr; },
            .open_all_settings = [this] { SetAllSettingsOpen(true); },
            .close_all_settings = [this] { SetAllSettingsOpen(false); },
            .all_settings_open = [this] { return all_settings_ != nullptr; },
            .open_instrument_lab =
                [this] {
                  app_context().CallInUIThreadDeferred([this] { PressBind("bind_instrument_lab"); });
                },
        });
    // UnregisterBind leaves the bind's cvar registered, and RegisterBind
    // would then log it as a duplicate: drop it too, and put the key back
    const std::string key = rex::cvar::GetFlagByName("bind_settings");
    rex::ui::UnregisterBind("bind_settings");
    rex::cvar::UnregisterFlag("bind_settings");
    rex::ui::RegisterBind("bind_settings", "F4",
                          "Open band3's settings; their All settings... opens the SDK's", [this] {
                            if (ingame_settings_ && !launcher_) ingame_settings_->Toggle();
                          });
    if (!key.empty() && rex::cvar::GetFlagByName("bind_settings") != key &&
        !rex::cvar::SetFlagByName("bind_settings", key)) {
      REXLOG_WARN("Settings: couldn't keep bind_settings = {}", key);
    }
  }

  // the SDK's settings menu: made and dropped outside the dialogs' draw,
  // since that's where "All settings..." asks
  void SetAllSettingsOpen(bool open) {
    app_context().CallInUIThreadDeferred([this, open] {
      if (open && !all_settings_ && imgui_drawer()) {
        all_settings_ =
            std::make_unique<rex::ui::SettingsDialog>(imgui_drawer(), path_defaults_.config_path);
        REXLOG_INFO("Settings: opened the SDK's settings menu");
      } else if (!open && all_settings_) {
        all_settings_.reset();
        REXLOG_INFO("Settings: closed the SDK's settings menu");
      }
    });
  }

  // presses whatever key a bind is set to, so the menu shortcut follows a
  // rebound F4 or F6
  void PressBind(const char* bind) {
    const auto key = rex::ui::ParseVirtualKey(rex::cvar::GetFlagByName(bind));
    if (key == rex::ui::VirtualKey::kNone || !window()) return;
    rex::ui::KeyEvent e(window(), key, 0, false, false, false, false, false);
    rex::ui::ProcessKeyEvent(e);
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
};
