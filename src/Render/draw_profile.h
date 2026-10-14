#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

// native_gpu_draw_profile: the world's GPU time draw by draw (a timestamp
// before each back-buffer world draw, gpu_timing_model.h's MarkKeyed), summed
// over frames by mesh and material, for the log. A draw's span runs from its
// timestamp to the next, so with the GPU overlapping draws it's roughly the
// time the draw added, not what it took alone; the spans still add up to the
// world's.

namespace band3::render {

// what a timed draw was: guest RndMesh and material pointers (named for the
// log by GuestObjectName), RndShader's ShaderType and option word, RndMat's
// blend and z mode, its triangles, skinned or not, and TheRnd's draw mode
struct DrawTag {
    uint32_t mesh = 0, mat = 0;
    int32_t shader_type = -1;
    uint64_t options = 0;
    int blend = 0, z_mode = 0;
    uint32_t tris = 0;
    bool skinned = false;
    uint8_t draw_mode = 0;
};

struct DrawTime {
    DrawTag tag;
    double ms = 0;
};

class DrawProfile {
 public:
    struct Row {
        DrawTag tag;  // the first draw seen of its mesh and material
        double ms = 0;
        uint64_t draws = 0;
    };

    // one frame's timed draws
    void AddFrame(const std::vector<DrawTime>& draws) {
        frames_++;
        for (const DrawTime& d : draws) {
            const uint64_t key = uint64_t(d.tag.mesh) << 32 | d.tag.mat;
            auto [it, made] = rows_.try_emplace(key);
            if (made) it->second.tag = d.tag;
            it->second.ms += d.ms;
            it->second.draws++;
            total_ms_ += d.ms;
        }
    }
    // the `n` with the most time, most first
    std::vector<Row> Top(size_t n) const {
        std::vector<Row> rows;
        rows.reserve(rows_.size());
        for (const auto& [key, row] : rows_) rows.push_back(row);
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.ms > b.ms; });
        if (rows.size() > n) rows.resize(n);
        return rows;
    }
    uint64_t Frames() const { return frames_; }
    double TotalMs() const { return total_ms_; }
    size_t Groups() const { return rows_.size(); }
    void Reset() {
        rows_.clear();
        frames_ = 0;
        total_ms_ = 0;
    }

 private:
    std::unordered_map<uint64_t, Row> rows_;
    uint64_t frames_ = 0;
    double total_ms_ = 0;
};

}  // namespace band3::render
