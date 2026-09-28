// band3 - ReXGlue Recompiled Project
//
// This file is yours to edit. 'rexglue migrate' will NOT overwrite it.
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <imgui.h>

#include "config.h"

class DebugOverlayDialog : public rex::ui::ImGuiDialog {
 public:
  explicit DebugOverlayDialog(rex::ui::ImGuiDrawer* imgui_drawer)
      : rex::ui::ImGuiDialog(imgui_drawer) {}

 protected:
  void OnDraw(ImGuiIO& io) override {
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

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Band3App>(new Band3App(ctx, "band3",
        PPCImageConfig));
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    band3::LoadConfig();
    auto& cfg = band3::GetConfig();
    paths.game_data_root = cfg.game_data_root;

    // runs before the window is created, so the size applies at startup
    if (cfg.width > 0 && cfg.height > 0) {
      rex::cvar::SetFlagByName("window_width", std::to_string(cfg.width));
      rex::cvar::SetFlagByName("window_height", std::to_string(cfg.height));
    }
  }

  void OnPostSetup() override {
    rex::cvar::SetFlagByName("log_level", band3::GetConfig().log_level);
    rex::cvar::SetFlagByName("audio_maxqframes", std::to_string(band3::GetConfig().max_queued_frames));
  }

  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    auto& cfg = band3::GetConfig();
    if (cfg.fullscreen) {
      window()->SetFullscreen(true);
    }

    if (cfg.debug_overlay && drawer) {
      debug_overlay_ = std::make_unique<DebugOverlayDialog>(drawer);
    } else {
      debug_overlay_.reset();
    }
  }

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnShutdown() override {}
};
