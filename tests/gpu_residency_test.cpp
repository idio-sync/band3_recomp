// Checks src/Render/gpu_view.h's residency rules, which GpuRenderer's Evict
// follows: under even/odd rendering the world's geometry and textures, drawn
// by one frame a world period, stay on the GPU through the period rather than
// being sent again by each post frame, and what's drawn once (a movie's
// frames) is still let go, a few frames later; and a texture pass's render
// target, forgotten as before, keeps its textures for a while, so a camera cut
// back to a character doesn't make its targets again.

#include <doctest/doctest.h>
#include <algorithm>
#include <cstdint>
#include <utility>
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

TEST_CASE("a render target is forgotten at kEvictAfter frames, released only once idle too") {
    constexpr double kKeepSeconds = 30;
    // within the frames it's kept as it is, however long ago that was (a
    // low frame rate)
    CHECK(KeepRt(10, 10, kEvict, 0, kKeepSeconds) == RtResidency::kKeep);
    CHECK(KeepRt(10, 10 + kEvict, kEvict, 0, kKeepSeconds) == RtResidency::kKeep);
    CHECK(KeepRt(10, 10 + kEvict, kEvict, 1000, kKeepSeconds) == RtResidency::kKeep);
    // a frame past them its picture is forgotten, where it used to be
    // released, but its textures stay until it's been idle the seconds
    CHECK(KeepRt(10, 11 + kEvict, kEvict, 0, kKeepSeconds) == RtResidency::kForget);
    CHECK(KeepRt(10, 11 + kEvict, kEvict, kKeepSeconds, kKeepSeconds) == RtResidency::kForget);
    CHECK(KeepRt(10, 11 + kEvict, kEvict, kKeepSeconds + 0.001, kKeepSeconds) ==
          RtResidency::kRelease);
    CHECK(KeepRt(10, 5000, kEvict, 1000, kKeepSeconds) == RtResidency::kRelease);
}

TEST_CASE("a character's targets out of the shot for a while aren't made again at the cut back") {
    // a target as GpuRenderer::Impl keeps it: made, drawn, last used
    struct Target {
        bool made = false;
        bool drawn = false;
        uint64_t used = 0;
        double used_at = 0;
    };
    // 120 frames a second; its pass is drawn in the shot, and not for `out`
    // seconds between two shots of it. What's made, and whether the frame
    // back in the shot found it drawn before its pass drew it.
    auto run = [](double out, int& made, bool& drawn_at_cut) {
        Target t;
        made = 0;
        const uint64_t out_from = 240, back = out_from + uint64_t(out * 120);
        for (uint64_t serial = 1; serial <= back + 10; serial++) {
            const double now = double(serial) / 120;
            if (serial < out_from || serial >= back) {
                if (serial == back) drawn_at_cut = t.drawn;
                if (!t.made) {
                    t.made = true;
                    made++;
                }
                t.drawn = true;
                t.used = serial;
                t.used_at = now;
            }
            switch (KeepRt(t.used, serial, kEvict, now - t.used_at, 30)) {
                case RtResidency::kKeep: break;
                case RtResidency::kForget: t.drawn = false; break;
                case RtResidency::kRelease: t = Target{}; break;
            }
        }
    };
    int made = 0;
    bool drawn_at_cut = true;
    // out for half a second: kept as it is, as before
    run(0.5, made, drawn_at_cut);
    CHECK(made == 1);
    CHECK(drawn_at_cut);
    // out for 5 seconds: forgotten (undrawn, as it was released before) but
    // not made again
    run(5, made, drawn_at_cut);
    CHECK(made == 1);
    CHECK_FALSE(drawn_at_cut);
    // out for 40: released, and made again
    run(40, made, drawn_at_cut);
    CHECK(made == 2);
    CHECK_FALSE(drawn_at_cut);
}

TEST_CASE("past the cap the forgotten targets least recently used are released") {
    // (used, DxTex)
    std::vector<std::pair<uint64_t, uint32_t>> forgotten = {
        {500, 1}, {300, 2}, {400, 3}, {300, 0}, {600, 4}};
    // at or under the cap, none
    CHECK(RtsOverCap(forgotten, 128, 128) == 0);
    CHECK(RtsOverCap(forgotten, 20, 128) == 0);
    // two over: the two used longest ago, ties by DxTex
    REQUIRE(RtsOverCap(forgotten, 130, 128) == 2);
    CHECK(forgotten[0] == std::pair<uint64_t, uint32_t>{300, 0});
    CHECK(forgotten[1] == std::pair<uint64_t, uint32_t>{300, 2});
    // further over than there are forgotten ones: all of them, oldest first
    // (the ones drawn within kEvictAfter stay)
    REQUIRE(RtsOverCap(forgotten, 200, 128) == forgotten.size());
    CHECK(forgotten[2].first == 400);
    CHECK(forgotten[3].first == 500);
    CHECK(forgotten[4].first == 600);
    std::vector<std::pair<uint64_t, uint32_t>> none;
    CHECK(RtsOverCap(none, 200, 128) == 0);
}
