#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

// Pointing the mouse at a menu's button or a list's row highlights it
// (mouse_menus): the geometry, kept apart from the game so it can be unit
// tested. src/Hooks/mouse_hover.cpp reads where the game drew the buttons and
// each list's rows and hands them here.
//
// RB3 draws a list's rows at regular steps from the list's origin
// (UIListDir::BuildDrawState), through the camera drawing it then. A point
// goes from world space to the screen as RndCam::WorldToScreen takes it on
// the Xbox 360 (rb3-xenon rndobj/Cam.cpp): times the camera's world-to-
// projection transform, divided by the result's z, then from -1..1 into the
// camera's screen rect (0..1 of the picture, y down).

namespace band3::input::mouse_hover {

struct Vec2 {
    float x = 0, y = 0;
};

struct Vec3 {
    float x = 0, y = 0, z = 0;
};

// Milo's Transform: rows x, y, z of the matrix, then the translation. A point
// is a row vector: v.x * x + v.y * y + v.z * z + translation.
struct Transform {
    std::array<Vec3, 3> rows{};
    Vec3 v;
};

// Hmx::Rect, in the picture's 0..1
struct Rect {
    float x = 0, y = 0, w = 1, h = 1;
};

Vec3 Apply(const Transform& t, const Vec3& p);

// a world point on the picture, 0..1 across and down (RB3's projections
// negate the camera's up, so y runs down the screen); nullopt behind the
// camera
std::optional<Vec2> Project(const Transform& world_project, const Rect& screen_rect,
                            const Vec3& world);

// where the picture lies in the window, in its pixels
struct PictureRect {
    int32_t x = 0, y = 0, w = 0, h = 0;
};

// a pointer at window pixel (x, y) on the picture, 0..1; nullopt off it
std::optional<Vec2> OnPicture(int32_t x, int32_t y, const PictureRect& picture);

// one drawn row, in the picture's pixels
struct Row {
    // which row of the list's data it shows (UIListState's showing index),
    // what SetSelectedSimulateScroll takes
    int showing = 0;
    Vec2 at;
    // a row that can be picked: active, and not fading in or out at the
    // list's ends, where the list scrolls on
    bool pickable = true;
};

struct Box2 {
    Vec2 min, max;
    bool Contains(const Vec2& p) const;
};

// The row under `pointer`, given the rows' anchors and the list's bounds on
// the picture (all in pixels): nullopt off the list, or on a row that can't
// be picked. A row's anchor is where the game puts it, which needn't be its
// middle, so the anchors are first moved together to centre them in the
// bounds; the row then is the one whose anchor is nearest. That's the
// band between the midpoints for a list, and the nearest cell for a grid.
std::optional<int> RowAt(const std::vector<Row>& rows, const Box2& bounds, const Vec2& pointer);

// how far RowAt moves the anchors to centre them in the bounds: a row's
// middle is its anchor plus this
Vec2 AnchorShift(const std::vector<Row>& rows, const Box2& bounds);

// Where a list's highlight can go without the list scrolling, from its
// UIListState: UIListState::BuildScroll moves the highlight on down to
// ScrollMaxDisplay() and up to mMinDisplay, and scrolls past them (up past
// mMinDisplay only once the list has scrolled, unless it scrolls past it
// anyway). A list at its end can't scroll on, so its last rows hold still. A
// circular list's highlight stays put while its rows move, so only it does.
struct ListState {
    bool circular = false;
    int num_display = 0;
    int min_display = 0;
    bool scroll_past_min = false;
    int scroll_max_display = 0;
    int first_showing = 0;
    int max_first_showing = 0;
    int selected_display = 0;
};

// whether highlighting the row drawn at `display` leaves the list where it is
bool StaysPut(const ListState& list, int display);

// A label's text in its own space, x across and z up, as
// UILabel::InqMinMaxFromWidthAndHeight puts it (the box its highlight frames):
// `alignment` is RndText::Alignment's bits, 1/2/4 left/centre/right and
// 0x10/0x20/0x40 top/middle/bottom, and the size the text's
std::array<Vec3, 4> LabelCorners(uint32_t alignment, float width, float height);

// the box around points on the picture
Box2 Around(const std::vector<Vec2>& points);

// a component's box on the picture, for picking the one under the pointer
struct Target {
    uint32_t component = 0;
    Box2 box;
};

// the target under `pointer`: the smallest box holding it, so a button
// inside a larger one wins; 0 for none
uint32_t TargetAt(const std::vector<Target>& targets, const Vec2& pointer);

// `from` and everything joined to it by `links` (from, to), either way, in
// the order found: the components a controller can move the focus to from
// `from`, given their down and right links; none for no focus (0)
std::vector<uint32_t> Joined(const std::vector<std::pair<uint32_t, uint32_t>>& links, uint32_t from);

}
