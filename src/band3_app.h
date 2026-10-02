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
#include <imgui.h>

#include "config.h"
#include "game_writes.h"
#include "paths.h"
#include "relaunch.h"
#include "settings.h"
#include "steam_deck.h"
#include "Audio/usb_mic_capture.h"
#include "Content/content_hooks.h"
#include "Content/live_content.h"
#include "Game/SongCache.h"
#include "Input/input_system.h"
#include "Input/instrument_lab.h"
#include "Input/menu_shortcut_dialog.h"
#include "Input/virtual_instrument.h"
#include "Launcher/game_data_check.h"
#include "Launcher/launcher_dialog.h"
#include "Launcher/launcher_start.h"
#include "Net/discord.h"
#include "Net/http_server.h"
#include "Render/gpu_view.h"
#include "Render/native_view.h"
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
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    }
    ImGui::End();
  }

 private:
  bool hidden_ = false;
};

class Band3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  std::unique_ptr<DebugOverlayDialog> debug_overlay_;
  std::unique_ptr<band3::input::InstrumentLabDialog> instrument_lab_;
  std::unique_ptr<band3::input::MenuShortcutDialog> menu_shortcut_;
  // the native view, see src/Render/native_view.h
  std::unique_ptr<band3::render::NativeViewDialog> native_view_;
  // the launcher while it's up, before the game starts (src/Launcher/)
  std::unique_ptr<band3::launcher::LauncherDialog> launcher_;
  // OnFinalizePaths' folders and resume, kept for the launcher's Play, and
  // what the path rule needs from the ini
  rex::PathConfig path_defaults_;
  std::function<void(rex::PathConfig)> resume_;
  std::filesystem::path anchor_;
  bool cache_in_ini_ = false;
  // quitting from the launcher was confirmed, so the window may close
  bool quit_confirmed_ = false;

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
    // a relaunch (rb3e_relaunch_game) starts before the last run has closed
    band3::relaunch::WaitForPrevious();
    LogFolders(game_data_root(), user_data_root(), cache_root());
    // before the ini, so a desktop ini's window settings don't undo them
    band3::steam_deck::ApplyDefaults();
    band3::ApplyLegacyIni();

    // band3 keeps a shorter audio queue than the SDK's 64 unless told otherwise
    if (rex::cvar::GetFlagSource("audio_maxqframes") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("audio_maxqframes", "3");
      rex::cvar::ClearPendingRestartFlags();
    }

#ifndef _WIN32
    if (rex::cvar::GetFlagByName("input_backend") == "xinput") {
      REXLOG_WARN("input_backend = xinput is Windows-only, using sdl");
      rex::cvar::SetFlagByName("input_backend", "sdl");
    }
#endif

    band3::AddSettingArgs();
    band3::settings::Init();
    band3::settings::SnapshotStartupSettings();
    band3::input::InitVirtualInstrument();
    band3::test::Init();
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
    auto setting = [](const char* cvar) {
      const Source source = rex::cvar::GetFlagSource(cvar);
      const PathSource from = source == Source::kDefault ? PathSource::kUnset
                              : source == Source::kConfig || source == Source::kRuntime
                                  ? PathSource::kSaved
                                  : PathSource::kFixed;
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
        .launcher_flag = REXCVAR_GET(launcher),
        .shift_held = band3::launcher::ShiftHeld(),
        .show_launcher = REXCVAR_GET(show_launcher),
    });
    // --launcher is for this start only; F4's "Save to config" would keep it
    if (REXCVAR_GET(launcher)) rex::cvar::SetFlagByName("launcher", "false");
    REXLOG_INFO("Launcher: {}", decision.reason);
    if (!decision.show) return paths;
    if (!imgui_drawer()) {
      REXLOG_WARN("Launcher: no ImGui to draw it with, starting the game");
      return paths;
    }

    resume_ = std::move(resume);
    launcher_ = std::make_unique<band3::launcher::LauncherDialog>(
        imgui_drawer(),
        band3::launcher::LauncherHost{
            .game_data_root = [this] { return RulePaths().game_data_root; },
            .start_game = [this] { StartFromLauncher(); },
            .quit = [this] { QuitFromLauncher(); },
        });
    if (debug_overlay_) debug_overlay_->set_hidden(true);
    return std::nullopt;
  }

  // the launcher's Play, once its "Starting" frame is drawn: the settings read
  // at startup are taken again from what the launcher left, then the game starts
  void StartFromLauncher() {
    band3::settings::SnapshotStartupSettings();
    band3::AddSettingArgs();
    // what the launcher changed is read from here on, so nothing waits on a
    // restart
    rex::cvar::ClearPendingRestartFlags();
    rex::PathConfig paths = FinalPaths();
    // not inside the launcher's draw: resume builds the runtime and starts the
    // native renderer on the same drawer
    app_context().CallInUIThreadDeferred([this, paths = std::move(paths)] {
      launcher_.reset();
      if (debug_overlay_) debug_overlay_->set_hidden(false);
      auto resume = std::move(resume_);
      resume(paths);
    });
  }

  void QuitFromLauncher() {
    quit_confirmed_ = true;
    app_context().CallInUIThreadDeferred([this] {
      if (window()) window()->RequestClose();
    });
  }

  // while the launcher has unsaved changes, closing the window asks first
  bool OnWindowCloseRequested() override {
    if (launcher_ && !quit_confirmed_ && launcher_->HasUnsavedChanges()) {
      launcher_->RequestQuit();
      return false;
    }
    return true;
  }

  // GPU emulation is a plugin (rexgpu-xenos) that the SDK leaves off unless
  // named; keep any gpu_plugin the user set in band3.toml
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // the SDK's input drivers plus band3's (virtual and PS3/Wii instruments)
    config.input_factory = band3::input::CreateInputSystem;
  }

  // before the game starts, so its mic threads find the microphones running
  // and its first file opens find game:\ writable
  void OnPostSetup() override {
    // a song cache rb3e_delete_songcache marked, before the game mounts it
    band3::song_cache::DeletePending(runtime()->user_data_root());
    band3::MountGameWrites(*runtime());
#ifdef _WIN32
    // band3's content overrides hand saves to the SDK's own exports; without
    // them the first save would fail, so fail here instead
    if (!band3::content::ResolveSdkContentExports()) {
      REXLOG_ERROR("content: the SDK's content exports are missing, can't continue");
      std::abort();
    }
#endif
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
    band3::http::StopServer();
    band3::test::StopServer();
    rex::ui::UnregisterBind("bind_instrument_lab");
    rex::ui::UnregisterBind("bind_native_view");
    rex::ui::UnregisterBind("bind_renderer");
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
      rex::ui::RegisterBind("bind_renderer", "F8",
                            "Switch between the emulated and the native renderer (experimental)", [] {
        rex::cvar::SetFlagByName("renderer",
                                 REXCVAR_GET(renderer) == "native" ? "emulated" : "native");
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
    } else {
      debug_overlay_.reset();
      instrument_lab_.reset();
      menu_shortcut_.reset();
      native_view_.reset();
    }
  }

  // presses whatever key a bind is set to, so the menu shortcut follows a
  // rebound F4 or F6 (the SDK keeps its settings menu to itself)
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
