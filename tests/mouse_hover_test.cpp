// Checks the geometry of hovering the mouse over a menu list
// (src/Input/mouse_hover.cpp): projecting a list's rows onto the picture,
// placing the pointer on it, and picking the row under the pointer.

#include <doctest/doctest.h>
#include <vector>
#include "src/Input/mouse_hover.h"

using namespace band3::input::mouse_hover;

namespace {

Transform Identity() {
    Transform t;
    t.rows = {Vec3{1, 0, 0}, Vec3{0, 1, 0}, Vec3{0, 0, 1}};
    return t;
}

// a vertical list's rows, 40 pixels apart from y 100, anchored at their left
// end and top (x 200), in a list 300 wide whose bounds start at the first anchor
std::vector<Row> Rows(int count) {
    std::vector<Row> rows;
    for (int i = 0; i < count; i++) rows.push_back({.showing = 10 + i, .at = {200, 100.0f + 40 * i}});
    return rows;
}

Box2 Bounds(int count) { return {{200, 100}, {500, 100.0f + 40 * count}}; }

}

TEST_CASE("a transform applies to a row vector, then adds its translation") {
    Transform t;
    t.rows = {Vec3{0, 1, 0}, Vec3{-1, 0, 0}, Vec3{0, 0, 2}};
    t.v = {10, 20, 30};
    const Vec3 p = Apply(t, {1, 2, 3});
    CHECK(p.x == doctest::Approx(10 - 2));
    CHECK(p.y == doctest::Approx(20 + 1));
    CHECK(p.z == doctest::Approx(30 + 6));
}

TEST_CASE("projection: -1..1 over z into the screen rect") {
    Transform t = Identity();
    // orthographic: RB3's leave z at 0, and nothing is divided
    t.rows[2] = {0, 0, 0};
    const Rect full;
    auto s = Project(t, full, {0.5f, -0.5f, 7});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(0.75f));
    CHECK(s->y == doctest::Approx(0.25f));
    t = Identity();
    t.v = {0, 0, 1};
    s = Project(t, full, {0, 0, 0});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(0.5f));
    CHECK(s->y == doctest::Approx(0.5f));
    s = Project(t, full, {-1, 1, 0});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(0.0f));
    CHECK(s->y == doctest::Approx(1.0f));

    // perspective: divided by depth, and a point behind is nowhere
    t.v = {0, 0, 0};
    s = Project(t, full, {2, 0, 4});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(0.75f));
    CHECK_FALSE(Project(t, full, {1, 1, -1}));

    // into a smaller screen rect
    t.v = {0, 0, 1};
    s = Project(t, Rect{0.5f, 0, 0.5f, 1}, {1, 0, 0});
    REQUIRE(s);
    CHECK(s->x == doctest::Approx(1.0f));
}

TEST_CASE("the pointer on the picture: 0..1 within its rect, nothing on the bars") {
    const PictureRect picture{160, 0, 1600, 900};
    auto p = OnPicture(160, 0, picture);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(0.5f / 1600));
    CHECK(p->y == doctest::Approx(0.5f / 900));
    p = OnPicture(160 + 800, 450, picture);
    REQUIRE(p);
    CHECK(p->x == doctest::Approx(0.5f).epsilon(0.001));
    CHECK_FALSE(OnPicture(100, 450, picture));
    CHECK_FALSE(OnPicture(1760, 450, picture));
    CHECK_FALSE(OnPicture(500, 500, PictureRect{}));
}

TEST_CASE("the row under the pointer, with anchors at the rows' top") {
    const auto rows = Rows(5);
    const Box2 bounds = Bounds(5);
    // the bounds run 100..300 and the anchors 100..260: rows are 100-140,
    // 140-180, ...
    CHECK(RowAt(rows, bounds, {300, 101}) == 10);
    CHECK(RowAt(rows, bounds, {300, 139}) == 10);
    CHECK(RowAt(rows, bounds, {300, 141}) == 11);
    CHECK(RowAt(rows, bounds, {450, 230}) == 13);
    CHECK(RowAt(rows, bounds, {300, 299}) == 14);
}

TEST_CASE("nothing off the list, or on a row that can't be picked") {
    auto rows = Rows(5);
    const Box2 bounds = Bounds(5);
    CHECK_FALSE(RowAt(rows, bounds, {150, 150}));
    CHECK_FALSE(RowAt(rows, bounds, {300, 90}));
    rows[0].pickable = false;
    CHECK_FALSE(RowAt(rows, bounds, {300, 120}));
    CHECK(RowAt(rows, bounds, {300, 150}) == 11);
    CHECK_FALSE(RowAt({}, bounds, {300, 150}));
    CHECK_FALSE(RowAt(rows, Box2{{200, 100}, {200, 300}}, {200, 150}));
}

TEST_CASE("a horizontal list and a grid pick the nearest cell") {
    std::vector<Row> across;
    for (int i = 0; i < 4; i++) across.push_back({.showing = i, .at = {100.0f + 100 * i, 50}});
    const Box2 strip{{50, 0}, {450, 100}};
    CHECK(RowAt(across, strip, {60, 50}) == 0);
    CHECK(RowAt(across, strip, {260, 90}) == 2);

    std::vector<Row> grid;
    for (int r = 0; r < 2; r++) {
        for (int c = 0; c < 3; c++) grid.push_back({.showing = r * 3 + c, .at = {100.0f * c, 100.0f * r}});
    }
    const Box2 cells{{-50, -50}, {250, 150}};
    CHECK(RowAt(grid, cells, {0, 0}) == 0);
    CHECK(RowAt(grid, cells, {190, 120}) == 5);
    CHECK(RowAt(grid, cells, {110, 40}) == 1);
}

TEST_CASE("a row's middle is its anchor moved as RowAt moves it") {
    const auto rows = Rows(5);
    const Vec2 shift = AnchorShift(rows, Bounds(5));
    CHECK(shift.x == doctest::Approx(150));
    CHECK(shift.y == doctest::Approx(20));
    const Vec2 middle{rows[3].at.x + shift.x, rows[3].at.y + shift.y};
    CHECK(RowAt(rows, Bounds(5), middle) == rows[3].showing);
}

TEST_CASE("a label's corners follow its text's alignment") {
    auto c = LabelCorners(0x11, 100, 20);  // top left: down and right of the origin
    CHECK(c[0].x == 0);
    CHECK(c[0].z == -20);
    CHECK(c[3].x == 100);
    CHECK(c[3].z == 0);
    c = LabelCorners(0x22, 100, 20);  // middle centre
    CHECK(c[0].x == -50);
    CHECK(c[0].z == -10);
    CHECK(c[3].x == 50);
    CHECK(c[3].z == 10);
    c = LabelCorners(0x44, 100, 20);  // bottom right
    CHECK(c[0].x == -100);
    CHECK(c[0].z == 0);
    CHECK(c[3].x == 0);
    CHECK(c[3].z == 20);
    for (const Vec3& corner : c) CHECK(corner.y == 0);
}

TEST_CASE("the target under the pointer is the smallest box holding it") {
    const Box2 around = Around({{10, 50}, {90, 20}, {40, 70}});
    CHECK(around.min.x == 10);
    CHECK(around.min.y == 20);
    CHECK(around.max.x == 90);
    CHECK(around.max.y == 70);
    const std::vector<Target> targets{{1, {{0, 0}, {100, 100}}}, {2, {{40, 40}, {60, 60}}},
                                      {3, {{200, 0}, {300, 100}}}};
    CHECK(TargetAt(targets, {50, 50}) == 2);
    CHECK(TargetAt(targets, {10, 10}) == 1);
    CHECK(TargetAt(targets, {250, 50}) == 3);
    CHECK(TargetAt(targets, {150, 50}) == 0);
}

TEST_CASE("the components joined to the focus, either way along their links") {
    // a menu 1-2-3 down, 3 right to 4, and a separate 7-8
    const std::vector<std::pair<uint32_t, uint32_t>> links{{1, 2}, {2, 3}, {3, 4}, {7, 8}};
    CHECK(Joined(links, 2) == std::vector<uint32_t>{2, 1, 3, 4});
    CHECK(Joined(links, 8) == std::vector<uint32_t>{8, 7});
    CHECK(Joined(links, 5) == std::vector<uint32_t>{5});
    CHECK(Joined({{1, 2}, {2, 1}}, 1) == std::vector<uint32_t>{1, 2});
    // no focus (a screen between panels): nothing, not a null component
    CHECK(Joined(links, 0).empty());
}

TEST_CASE("a list's highlight moves without scrolling only within its band") {
    // the music library: 12 rows, scrolling once the highlight passes row 7
    ListState list{.num_display = 12, .min_display = 3, .scroll_max_display = 7,
                   .max_first_showing = 80};
    // at the top: every row down to 7 holds still
    CHECK(StaysPut(list, 0));
    CHECK(StaysPut(list, 7));
    CHECK_FALSE(StaysPut(list, 8));
    CHECK_FALSE(StaysPut(list, 11));
    // scrolled down: rows above the minimum scroll it back up
    list.first_showing = 10;
    CHECK_FALSE(StaysPut(list, 2));
    CHECK(StaysPut(list, 3));
    // at the end: the last rows hold still
    list.first_showing = 80;
    CHECK(StaysPut(list, 11));
    // scrolling past the minimum even at the top
    list.first_showing = 0;
    list.scroll_past_min = true;
    CHECK_FALSE(StaysPut(list, 1));
    // a circular list holds still only on its highlight
    const ListState wheel{.circular = true, .num_display = 5, .selected_display = 2};
    CHECK(StaysPut(wheel, 2));
    CHECK_FALSE(StaysPut(wheel, 3));
}
