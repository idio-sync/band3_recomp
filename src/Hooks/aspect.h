#pragma once

#include <cstdint>

namespace band3::aspect {

// the window's client size, and whether the game's cameras fill it: the
// native present's (native_view.cpp), native_fill_window as it starts and
// at each paint, false as it stops. A size of 0 (minimized) keeps what was
// there. aspect.cpp builds the game's cameras for it (aspect_model.h).
void SetWindow(uint32_t width, uint32_t height, bool fill);

// before RndCam::Select(cam): has it rebuild a camera built for another shape
void BeforeSelect(uint8_t* base, uint32_t cam);

// OverlayEdge for the shape the cameras are built for now
void CurrentOverlayEdge(float out[2]);

}  // namespace band3::aspect
