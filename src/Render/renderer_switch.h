#pragma once

#include <functional>
#include <string_view>
#include "renderer_mode.h"

// The renderer setting at run time (rules in renderer_mode.h), following F4,
// the launcher, the harness's `set` and F8. The shown picture lives here, not
// in the setting: F8 switches it under `both` and leaves renderer alone, so a
// save keeps the player's choice.

namespace band3::render {

// Once, from Band3App::OnPreSetup, before anything reads the picture.
// Follows renderer from then on.
void StartRendererSwitch(const StartupGpuPlan& plan, bool presentable);

RendererState CurrentRenderer();

// whether the window shows the native renderer's picture now. Any thread.
bool ShowsNativePicture();

// F8 (bind_renderer)
void PressRendererSwitch();

// whether `setting` needs another GPU than this run's (only a restart can)
bool RendererRestartNeeded(std::string_view setting);

// Called each time the shown picture changes, on the thread that changed it
// (the UI thread). Returns an id for RemoveShownPictureListener.
int AddShownPictureListener(std::function<void(bool native)> listener);
void RemoveShownPictureListener(int id);

}  // namespace band3::render
