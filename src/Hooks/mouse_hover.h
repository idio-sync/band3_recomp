#pragma once
#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "src/Input/mouse_hover.h"

struct PPCContext;

// Pointing the mouse at a menu's button or a list's row highlights it
// (mouse_menus), the game's half: where the buttons and lists' rows were
// drawn, and the one under the pointer highlighted. Input/mouse_hover.h has
// the geometry, Input/mouse_menus_driver.h the pointer.

namespace band3::mouse_hover {

// Game thread, each frame from App::DrawRegular, before it draws
void RunFrame(PPCContext& ctx, uint8_t* base);

// What the hover sees of the focused component, for the test harness's
// `mouse rows`: in the window's pixels.
struct ReportRow {
    int display = 0;  // its place among the rows drawn, from the top (or left)
    int showing = 0;
    int data = 0;
    input::mouse_hover::Vec2 at;
    bool pickable = false;
    bool highlighted = false;
};

struct Report {
    std::string screen;
    // the focused component is a list drawn last frame
    bool list = false;
    input::mouse_hover::PictureRect picture;
    // the list's bounds, and its rows' anchors
    input::mouse_hover::Box2 bounds;
    std::vector<ReportRow> rows;
    // the pointer moves the game has acted on
    uint64_t moves = 0;
    // the focused component and the lists drawn last frame, each as its
    // address and class
    std::string focus;
    std::vector<std::string> drawn;
    // the focused panel's components the pointer can move the focus to, and
    // their boxes
    std::vector<std::pair<std::string, input::mouse_hover::Box2>> targets;
};

// Any thread but the game's: the next frame's view, for a window of this
// size; nullopt if the game draws no frame within `wait`
std::optional<Report> RequestReport(uint32_t width, uint32_t height, std::chrono::milliseconds wait);

}
