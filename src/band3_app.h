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
#include "relaunch.h"
#include "settings.h"
#include "steam_deck.h"
#include "Audio/usb_mic_capture.h"
#include "Game/SongCache.h"
#include "Input/input_system.h"
#include "Input/instrument_lab.h"
#include "Input/menu_shortcut_dialog.h"
#include "Input/virtual_instrument.h"
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

 protected:
  void OnDraw(ImGuiIO& io) override {
    if (!REXCVAR_GET(debug_overlay)) return;
    ImGui::SetNextWindowPos(ImVec2(10, 10), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(ImVec2(220, 60), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowBgAlpha(0.5f);
    if (ImGui::Begin("Debug##overlay", nullptr, ImGuiWindowFlags_NoCollapse)) {
      ImGui::Text("%.1f FPS (%.2f ms)", io.Framerate, 1000.0f / io.Framerate);
    }
    ImGui::End();
  }
};

class Band3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;
  std::unique_ptr<DebugOverlayDialog> debug_overlay_;
  std::unique_ptr<band3::input::InstrumentLabDialog> instrument_lab_;
  std::unique_ptr<band3::input::MenuShortcutDialog> menu_shortcut_;
  // the native view, see src/Render/native_view.h
  std::unique_ptr<band3::render::NativeViewDialog> native_view_;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Band3App>(new Band3App(ctx, "band3",
        PPCImageConfig));
  }

  // paths are fixed before band3.toml loads, so the game data root is the one
  // setting read here; --game_data_root on the command line wins over the ini
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (rex::cvar::GetFlagSource("game_data_root") == rex::cvar::Source::kDefault) {
      paths.game_data_root = band3::ReadIniGameDataRoot();
    }
    band3::SetGameDataRoot(paths.game_data_root.string());
  }

  // band3.toml, the environment and the command line are applied by now, and
  // the window and input system don't exist yet, so everything set here applies
  // at startup
  void OnPostInitLogging() override {
    // a relaunch (rb3e_relaunch_game) starts before the last run has closed
    band3::relaunch::WaitForPrevious();
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
    band3::input::InitVirtualInstrument();
    band3::test::Init();
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
    band3::discord::Start();
    band3::audio::StartUsbMics();
    band3::render::StartDumpIfRequested();
    band3::test::StartServer(runtime(), &app_context(), window());
    band3::http::StartServer();
  }

  void OnShutdown() override {
    band3::http::StopServer();
    band3::test::StopServer();
    rex::ui::UnregisterBind("bind_instrument_lab");
    rex::ui::UnregisterBind("bind_native_view");
    band3::render::StopNativeView();
    // the F7 window's worker would draw on the CPU once the GPU is gone
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
      rex::ui::RegisterBind("bind_instrument_lab", "F6", "Toggle the Instrument Lab", [this] {
        if (instrument_lab_) instrument_lab_->Toggle();
      });
      native_view_ = std::make_unique<band3::render::NativeViewDialog>(
          drawer, [this] { return immediate_drawer(); });
      rex::ui::RegisterBind("bind_native_view", "F7", "Toggle the native view (experimental)", [this] {
        if (native_view_) native_view_->Toggle();
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
