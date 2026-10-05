// Checks src/Render/gpu_view.h's residency rules, which GpuRenderer's Evict
// follows: under even/odd rendering the world's geometry and textures, drawn
// by one frame a world period, stay on the GPU through the period rather than
// being sent again by each post frame, and what's drawn once (a movie's
// frames) is still let go, a few frames later.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <vector>
#include "src/Render/gpu_view.h"

using namespace band3::render;

namespace {

constexpr uint64_t kEvict = 120;

// one mesh's or texture's bookkeeping, as GpuRenderer::Impl keeps it
struct Held {
    uint64_t first = 0, used = 0;
    bool in_arena = false;
    bool sent_from_cpu = false;  // the last time it was drawn
};

// frame `serial` draws `h`: as Render's use_mesh does
void Draw(Held& h, uint64_t serial) {
    h.sent_from_cpu = false;
    if (!h.first) {
        h.first = serial;
        h.sent_from_cpu = true;  // into the frame's pool
    } else if (!h.in_arena) {
        h.sent_from_cpu = !MeshFromLastPool(h.first, serial);
        h.in_arena = true;
    }
    h.used = serial;
}

}  // namespace

TEST_CASE("the keep covers the world period, or nothing when the world is drawn every frame") {
    CHECK(ResidencyKeepFrames(0) == 0);
    CHECK(ResidencyKeepFrames(1) == 0);
    // longer than the frames between two post frames, with room for a
    // frame's world passes and one drawn twice
    CHECK(ResidencyKeepFrames(4) > 4);
    CHECK(ResidencyKeepFrames(6) > 6);
}

TEST_CASE("the world's meshes drawn every fourth frame are sent once, then kept") {
    const uint64_t keep = ResidencyKeepFrames(4);
    Held mesh;
    int sent = 0;
    bool evicted = false;
    // a post frame every fourth serial draws it; the frames between don't
    for (uint64_t serial = 10; serial < 200 && !evicted; serial++) {
        if (serial % 4 == 2) {
            Draw(mesh, serial);
            sent += mesh.sent_from_cpu;
        }
        evicted = !KeepMesh(mesh.used, mesh.in_arena, serial, keep, kEvict);
    }
    CHECK_FALSE(evicted);
    CHECK(mesh.in_arena);
    // into the pool first, then once more into the arena: its pool has been
    // drawn over by the time the next post frame draws it
    CHECK(sent == 2);

    // without the keep (as before): let go after each, sent by every post frame
    Held before;
    for (uint64_t serial = 10; serial < 30; serial++) {
        if (serial % 4 == 2) {
            Draw(before, serial);
            CHECK(before.sent_from_cpu);
        }
        if (!KeepMesh(before.used, before.in_arena, serial, 0, kEvict)) before = Held{};
    }
}

TEST_CASE("a mesh the next frame draws again moves from its pool on the GPU") {
    Held mesh;
    Draw(mesh, 5);
    Draw(mesh, 6);
    CHECK(mesh.in_arena);
    CHECK_FALSE(mesh.sent_from_cpu);
    // and one in the arena stays kEvictAfter frames undrawn
    CHECK(KeepMesh(6, true, 6 + kEvict, 0, kEvict));
    CHECK_FALSE(KeepMesh(6, true, 7 + kEvict, 0, kEvict));
}

TEST_CASE("textures drawn once, a movie's frames, stay bounded") {
    const uint64_t keep = ResidencyKeepFrames(4);
    struct Tex {
        uint64_t first, used;
    };
    std::vector<Tex> resident;
    size_t most = 0;
    // each frame draws a new one and never again
    for (uint64_t serial = 1; serial < 500; serial++) {
        resident.push_back({serial, serial});
        std::erase_if(resident, [&](const Tex& t) {
            return !KeepTexture(t.first, t.used, serial, keep, kEvict);
        });
        most = std::max(most, resident.size());
    }
    CHECK(most == keep + 1);
    // a texture drawn by two frames counts as kept for kEvictAfter
    CHECK(KeepTexture(10, 14, 14 + kEvict, keep, kEvict));
    CHECK_FALSE(KeepTexture(10, 14, 15 + kEvict, keep, kEvict));
    // and one drawn once goes `keep` frames after
    CHECK(KeepTexture(10, 10, 10 + keep, keep, kEvict));
    CHECK_FALSE(KeepTexture(10, 10, 11 + keep, keep, kEvict));
}
