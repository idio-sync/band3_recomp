#include "mouse_hover.h"
#include <algorithm>
#include <limits>

namespace band3::input::mouse_hover {

Vec3 Apply(const Transform& t, const Vec3& p) {
    const auto& [x, y, z] = t.rows;
    return {p.x * x.x + p.y * y.x + p.z * z.x + t.v.x,
            p.x * x.y + p.y * y.y + p.z * z.y + t.v.y,
            p.x * x.z + p.y * y.z + p.z * z.z + t.v.z};
}

std::optional<Vec2> Project(const Transform& world_project, const Rect& screen_rect,
                            const Vec3& world) {
    const Vec3 p = Apply(world_project, world);
    // a perspective camera's z is the depth, and WorldToScreen divides by it;
    // an orthographic one's is 0, and it doesn't
    if (p.z < 0) return std::nullopt;
    const float depth = p.z > 0 ? p.z : 1.0f;
    const float x = (p.x / depth + 1.0f) * 0.5f;
    const float y = (p.y / depth + 1.0f) * 0.5f;
    return Vec2{x * screen_rect.w + screen_rect.x, y * screen_rect.h + screen_rect.y};
}

std::optional<Vec2> OnPicture(int32_t x, int32_t y, const PictureRect& picture) {
    if (picture.w <= 0 || picture.h <= 0) return std::nullopt;
    if (x < picture.x || y < picture.y || x >= picture.x + picture.w || y >= picture.y + picture.h) {
        return std::nullopt;
    }
    return Vec2{(float(x - picture.x) + 0.5f) / float(picture.w),
                (float(y - picture.y) + 0.5f) / float(picture.h)};
}

bool Box2::Contains(const Vec2& p) const {
    return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y;
}

Vec2 AnchorShift(const std::vector<Row>& rows, const Box2& bounds) {
    if (rows.empty()) return {};
    Box2 anchors{rows[0].at, rows[0].at};
    for (const Row& row : rows) {
        anchors.min.x = std::min(anchors.min.x, row.at.x);
        anchors.min.y = std::min(anchors.min.y, row.at.y);
        anchors.max.x = std::max(anchors.max.x, row.at.x);
        anchors.max.y = std::max(anchors.max.y, row.at.y);
    }
    return {(bounds.min.x + bounds.max.x - anchors.min.x - anchors.max.x) * 0.5f,
            (bounds.min.y + bounds.max.y - anchors.min.y - anchors.max.y) * 0.5f};
}

bool StaysPut(const ListState& list, int display) {
    if (list.circular) return display == list.selected_display;
    const int lo = list.scroll_past_min || list.first_showing > 0 ? list.min_display : 0;
    const int hi = list.first_showing >= list.max_first_showing ? list.num_display - 1
                                                                 : list.scroll_max_display;
    return display >= lo && display <= hi;
}

std::array<Vec3, 4> LabelCorners(uint32_t alignment, float width, float height) {
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
    if (alignment & 1) {
        x1 = width;
    } else if (alignment & 2) {
        x0 = -width / 2;
        x1 = width / 2;
    } else if (alignment & 4) {
        x0 = -width;
    }
    if (alignment & 0x10) {
        z0 = -height;
    } else if (alignment & 0x20) {
        z0 = -height / 2;
        z1 = height / 2;
    } else if (alignment & 0x40) {
        z1 = height;
    }
    return {Vec3{x0, 0, z0}, Vec3{x1, 0, z0}, Vec3{x0, 0, z1}, Vec3{x1, 0, z1}};
}

Box2 Around(const std::vector<Vec2>& points) {
    if (points.empty()) return {};
    Box2 box{points[0], points[0]};
    for (const Vec2& p : points) {
        box.min.x = std::min(box.min.x, p.x);
        box.min.y = std::min(box.min.y, p.y);
        box.max.x = std::max(box.max.x, p.x);
        box.max.y = std::max(box.max.y, p.y);
    }
    return box;
}

uint32_t TargetAt(const std::vector<Target>& targets, const Vec2& pointer) {
    uint32_t best = 0;
    float best_area = std::numeric_limits<float>::infinity();
    for (const Target& t : targets) {
        if (!t.box.Contains(pointer)) continue;
        const float area = (t.box.max.x - t.box.min.x) * (t.box.max.y - t.box.min.y);
        if (area < best_area) {
            best_area = area;
            best = t.component;
        }
    }
    return best;
}

std::vector<uint32_t> Joined(const std::vector<std::pair<uint32_t, uint32_t>>& links, uint32_t from) {
    if (!from) return {};
    std::vector<uint32_t> joined{from};
    for (size_t i = 0; i < joined.size(); i++) {
        for (const auto& [a, b] : links) {
            const uint32_t next = a == joined[i] ? b : b == joined[i] ? a : 0;
            if (next && std::find(joined.begin(), joined.end(), next) == joined.end()) {
                joined.push_back(next);
            }
        }
    }
    return joined;
}

std::optional<int> RowAt(const std::vector<Row>& rows, const Box2& bounds, const Vec2& pointer) {
    if (rows.empty() || !(bounds.max.x > bounds.min.x) || !(bounds.max.y > bounds.min.y)) {
        return std::nullopt;
    }
    if (!bounds.Contains(pointer)) return std::nullopt;

    const Vec2 shift = AnchorShift(rows, bounds);

    const Row* nearest = nullptr;
    float best = std::numeric_limits<float>::infinity();
    for (const Row& row : rows) {
        const float dx = row.at.x + shift.x - pointer.x;
        const float dy = row.at.y + shift.y - pointer.y;
        const float d = dx * dx + dy * dy;
        if (d < best) {
            best = d;
            nearest = &row;
        }
    }
    if (!nearest || !nearest->pickable) return std::nullopt;
    return nearest->showing;
}

}
