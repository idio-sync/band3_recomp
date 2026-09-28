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
#include "settings.h"
#include "Input/instrument_lab.h"
#include "Input/virtual_instrument.h"
#include "Net/discord.h"

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
  }

  // GPU emulation is a plugin (rexgpu-xenos) that the SDK leaves off unless
  // named; keep any gpu_plugin the user set in band3.toml
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    // the SDK's input drivers plus the Instrument Lab's virtual instrument
    config.input_factory = band3::input::CreateInputSystem;
  }

  void OnPostSetup() override {
    band3::discord::Start();
  }

  void OnShutdown() override {
    rex::ui::UnregisterBind("bind_instrument_lab");
    band3::discord::Stop();
  }

  // the SDK applies the fullscreen cvar to the window itself
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    if (drawer) {
      debug_overlay_ = std::make_unique<DebugOverlayDialog>(drawer);
      instrument_lab_ = std::make_unique<band3::input::InstrumentLabDialog>(drawer);
      rex::ui::RegisterBind("bind_instrument_lab", "F6", "Toggle the Instrument Lab", [this] {
        if (instrument_lab_) instrument_lab_->Toggle();
      });
    } else {
      debug_overlay_.reset();
      instrument_lab_.reset();
    }
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
};
