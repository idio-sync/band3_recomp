#pragma once

#include <functional>
#include <string_view>
#include "renderer_mode.h"

// The renderer setting while band3 runs (renderer_mode.h has the rules): the
// run startup planned, the setting followed as F4, the launcher and the
// harness's `set` change it, and F8. The picture the window shows is kept
// here, not in the setting: F8 switches it in both and leaves renderer as it
// is, so a save keeps the player's choice.

namespace band3::render {

// Once, from Band3App::OnPreSetup, before anything reads the picture: the run
// startup planned. Follows renderer from then on.
void StartRendererSwitch(const StartupGpuPlan& plan, bool presentable);

RendererState CurrentRenderer();

// whether the window shows the native renderer's picture now. Any thread.
bool ShowsNativePicture();

// F8 (bind_renderer)
void PressRendererSwitch();

// whether `setting` (renderer's value, e.g. from the launcher) needs another
// GPU than this run's, which only a restart brings
bool RendererRestartNeeded(std::string_view setting);

// Called with the picture shown each time it changes, on the thread that
// changed it (the UI thread: F4, F8, the launcher, the harness). Returns an id
// for RemoveShownPictureListener.
int AddShownPictureListener(std::function<void(bool native)> listener);
void RemoveShownPictureListener(int id);

}  // namespace band3::render
