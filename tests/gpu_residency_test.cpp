// Checks src/Render/gpu_view.h's residency rules, which GpuRenderer's Evict
// follows: under even/odd rendering the world's geometry and textures, drawn
// by one frame a world period, stay on the GPU through the period rather than
// being sent again by each post frame, and what's drawn once (a movie's
// frames) is still let go, a few frames later; a texture pass's render
// target, forgotten as before, keeps its textures for a while, so a camera cut
// back to a character doesn't make its targets again; and the character's
// meshes and textures are kept by the clock too, so the cut back doesn't send
// them again, within bounds on the memory that keeps.

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
        evicted = !KeepMesh(mesh.used, mesh.in_arena, serial, keep, kEvict, 0, 0);
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
        if (!KeepMesh(before.used, before.in_arena, serial, 0, kEvict, 0, 0)) before = Held{};
    }
}

TEST_CASE("a mesh the next frame draws again moves from its pool on the GPU") {
    Held mesh;
    Draw(mesh, 5);
    Draw(mesh, 6);
    CHECK(mesh.in_arena);
    CHECK_FALSE(mesh.sent_from_cpu);
    // and one in the arena stays kEvictAfter frames undrawn (by the frames
    // alone, keep_seconds 0, as when short of room)
    CHECK(KeepMesh(6, true, 6 + kEvict, 0, kEvict, 0, 0));
    CHECK_FALSE(KeepMesh(6, true, 7 + kEvict, 0, kEvict, 0, 0));
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
            return !KeepTexture(t.first, t.used, serial, keep, kEvict, 0, 0);
        });
        most = std::max(most, resident.size());
    }
    CHECK(most == keep + 1);
    // a texture drawn by two frames counts as kept for kEvictAfter
    CHECK(KeepTexture(10, 14, 14 + kEvict, keep, kEvict, 0, 0));
    CHECK_FALSE(KeepTexture(10, 14, 15 + kEvict, keep, kEvict, 0, 0));
    // and one drawn once goes `keep` frames after
    CHECK(KeepTexture(10, 10, 10 + keep, keep, kEvict, 0, 0));
    CHECK_FALSE(KeepTexture(10, 10, 11 + keep, keep, kEvict, 0, 0));
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

TEST_CASE("what's drawn in more than one frame goes once the frames and the seconds have passed") {
    constexpr double kKeepSeconds = 30;
    // within the frames: kept, however long ago (a low frame rate)
    CHECK(WithinKeep(10, 10 + kEvict, kEvict, 1000, kKeepSeconds));
    // past the frames but not the seconds: kept, where it used to go
    CHECK(WithinKeep(10, 11 + kEvict, kEvict, 0, kKeepSeconds));
    CHECK(WithinKeep(10, 11 + kEvict, kEvict, kKeepSeconds, kKeepSeconds));
    // both past: let go
    CHECK_FALSE(WithinKeep(10, 11 + kEvict, kEvict, kKeepSeconds + 0.001, kKeepSeconds));
    // keep_seconds 0, short of room: the frames alone, as before
    CHECK(WithinKeep(10, 10 + kEvict, kEvict, 0, 0));
    CHECK_FALSE(WithinKeep(10, 11 + kEvict, kEvict, 0, 0));

    // a mesh in the arena and a texture drawn in two frames the same way
    CHECK(KeepMesh(10, true, 11 + kEvict, 0, kEvict, 5, kKeepSeconds));
    CHECK_FALSE(KeepMesh(10, true, 11 + kEvict, 0, kEvict, 31, kKeepSeconds));
    CHECK(KeepTexture(5, 10, 11 + kEvict, 0, kEvict, 5, kKeepSeconds));
    CHECK_FALSE(KeepTexture(5, 10, 11 + kEvict, 0, kEvict, 31, kKeepSeconds));
    // and whatever drew it this frame keeps it
    CHECK(KeepMesh(10, true, 10, 0, kEvict, 1000, kKeepSeconds));
}

TEST_CASE("what's drawn in one frame only isn't kept by the clock") {
    constexpr double kKeepSeconds = 30;
    const uint64_t keep = ResidencyKeepFrames(4);
    // a mesh in its pool, never moved to the arena: `keep` frames, however
    // recently by the clock
    CHECK(KeepMesh(10, false, 10 + keep, keep, kEvict, 0, kKeepSeconds));
    CHECK_FALSE(KeepMesh(10, false, 11 + keep, keep, kEvict, 0, kKeepSeconds));
    // a texture drawn once (first == used): a movie's frame
    CHECK(KeepTexture(10, 10, 10 + keep, keep, kEvict, 0, kKeepSeconds));
    CHECK_FALSE(KeepTexture(10, 10, 11 + keep, keep, kEvict, 0, kKeepSeconds));
    // so a movie's frames at 120 a second stay as bounded as before
    struct Tex {
        uint64_t first, used;
        double used_at;
    };
    std::vector<Tex> resident;
    size_t most = 0;
    for (uint64_t serial = 1; serial < 500; serial++) {
        const double now = double(serial) / 120;
        resident.push_back({serial, serial, now});
        std::erase_if(resident, [&](const Tex& t) {
            return !KeepTexture(t.first, t.used, serial, keep, kEvict, now - t.used_at,
                                kKeepSeconds);
        });
        most = std::max(most, resident.size());
    }
    CHECK(most == keep + 1);
}

TEST_CASE("a character's meshes out of the shot for a while aren't sent again at the cut back") {
    constexpr double kKeepSeconds = 30;
    // 120 frames a second; the mesh, one frames of many worlds draw (kept by
    // ClockKeep's keep_seconds), is drawn in the shot, and not for `out`
    // seconds between two shots of it: how often it's sent from the CPU
    auto sent = [&](double out, double keep_seconds) {
        Held mesh;
        double used_at = 0;
        int n = 0;
        const uint64_t out_from = 240, back = out_from + uint64_t(out * 120);
        for (uint64_t serial = 1; serial <= back + 10; serial++) {
            const double now = double(serial) / 120;
            if (serial < out_from || serial >= back) {
                Draw(mesh, serial);
                n += mesh.sent_from_cpu;
                used_at = now;
            }
            if (!KeepMesh(mesh.used, mesh.in_arena, serial, 0, kEvict, now - used_at,
                          keep_seconds))
                mesh = Held{};
        }
        return n;
    };
    // into the pool, then moved to the arena on the GPU: once
    CHECK(sent(0.5, kKeepSeconds) == 1);
    CHECK(sent(5, kKeepSeconds) == 1);
    CHECK(sent(29, kKeepSeconds) == 1);
    // by the frames alone (as before, or short of room) out for 5 s it's
    // let go and sent again
    CHECK(sent(5, 0) == 2);
    // and out for 40 s it goes anyway
    CHECK(sent(40, kKeepSeconds) == 2);
}

TEST_CASE("what's made anew each game frame isn't kept by the clock when its world is drawn twice") {
    constexpr double kKeepSeconds = 30;
    // in a song, last drawn in it: kept by the clock only if frames of more
    // than one world drew it
    CHECK(ClockKeep(true, true, true, kKeepSeconds) == kKeepSeconds);
    CHECK(ClockKeep(true, true, false, kKeepSeconds) == 0);
    // Even/odd at 120 Hz, a world period of 4: a world frame draws the
    // world, the post frame after it draws that world again (composed with
    // it), two frames between show the kept post buffer. Each world has a
    // particle system's geometry of its own, which both its frames draw, so
    // it moves to the arena; a character's mesh is the same in every world.
    struct Mesh {
        Held held;
        uint64_t first_world = 0;
        bool across_worlds = false;
        double used_at = 0;
    };
    const uint64_t keep = ResidencyKeepFrames(4);
    std::vector<Mesh> particles;
    Mesh character;
    bool character_kept = true;
    size_t most = 0;
    for (uint64_t serial = 1; serial < 4000; serial++) {
        const double now = double(serial) / 120;
        const uint64_t world = (serial + 3) / 4;  // the world's game frame
        auto draw = [&](Mesh& m) {
            if (!m.held.first) m.first_world = world;
            if (world != m.first_world) m.across_worlds = true;
            Draw(m.held, serial);
            m.used_at = now;
        };
        if (serial % 4 == 1) particles.push_back({});
        if (serial % 4 == 1 || serial % 4 == 2) {
            draw(particles.back());
            draw(character);
        }
        std::erase_if(particles, [&](const Mesh& m) {
            return !KeepMesh(m.held.used, m.held.in_arena, serial, keep, kEvict, now - m.used_at,
                             ClockKeep(true, true, m.across_worlds, kKeepSeconds));
        });
        most = std::max(most, particles.size());
        character_kept &= KeepMesh(character.held.used, character.held.in_arena, serial, keep,
                                   kEvict, now - character.used_at,
                                   ClockKeep(true, true, character.across_worlds, kKeepSeconds));
    }
    CHECK(character_kept);
    // the particles in the arena go at kEvictAfter frames, as before: about
    // a world's for every 4 of them, not 30 s of worlds (900)
    CHECK(most <= kEvict / 4 + 2);
    CHECK(character.across_worlds);
    CHECK(character.held.in_arena);
}

TEST_CASE("out of a song, and for what was last drawn out of one, the frames alone as before") {
    constexpr double kKeepSeconds = 30;
    // not in a song (boot, the menus): no clock, whatever drew it
    CHECK(ClockKeep(false, true, true, kKeepSeconds) == 0);
    CHECK(ClockKeep(false, false, true, kKeepSeconds) == 0);
    // in a song, but last drawn before it (the menus' last frames): none, so
    // a song's loading lets the menus' textures go as it did
    CHECK(ClockKeep(true, false, true, kKeepSeconds) == 0);

    // With keep_seconds 0, KeepMesh and KeepTexture are exactly what they
    // were before the clock: kept if drawn in the frame, or within
    // `evict_after` frames in the arena (drawn in more than one frame), or
    // else within `keep`; however long ago by the clock
    auto before = [](uint64_t used, bool in_arena, uint64_t serial, uint64_t keep) {
        return used == serial || used + (in_arena ? kEvict : keep) >= serial;
    };
    const uint64_t keep = ResidencyKeepFrames(4);
    bool same = true;
    for (uint64_t used = 1; used < 300; used += 7)
        for (uint64_t serial = used; serial < used + 2 * kEvict; serial++)
            for (bool in_arena : {false, true})
                for (double idle : {0.0, 1.0, 29.0, 31.0, 1000.0}) {
                    const double none = ClockKeep(false, true, true, kKeepSeconds);
                    same &= KeepMesh(used, in_arena, serial, keep, kEvict, idle, none) ==
                            before(used, in_arena, serial, keep);
                    const uint64_t first = in_arena ? used - 1 : used;
                    same &= KeepTexture(first, used, serial, keep, kEvict, idle, none) ==
                            before(used, in_arena, serial, keep);
                }
    CHECK(same);
}

TEST_CASE("an arena rebuild keeps what's within its keep, up to the cap") {
    constexpr uint64_t kCap = 256u << 20;
    constexpr double kKeepSeconds = 30;
    // twice what it keeps fits: all of it
    CHECK(ArenaRebuildKeep(100u << 20, 50u << 20, kCap) == ArenaKeep::kKeep);
    CHECK(ArenaRebuildKeep(128u << 20, 50u << 20, kCap) == ArenaKeep::kKeep);
    // it doesn't, but the frames' does
    CHECK(ArenaRebuildKeep((128u << 20) + 1, 128u << 20, kCap) == ArenaKeep::kFrames);
    // neither: only what the frame draws, as before the clock kept them
    CHECK(ArenaRebuildKeep(200u << 20, (128u << 20) + 1, kCap) == ArenaKeep::kFrame);

    // a mesh drawn this frame is kept by any of them
    for (ArenaKeep what : {ArenaKeep::kKeep, ArenaKeep::kFrames, ArenaKeep::kFrame})
        CHECK(KeptByRebuild(what, 500, 500, kEvict, 0, kKeepSeconds));
    // one drawn within the frames, by the first two
    CHECK(KeptByRebuild(ArenaKeep::kKeep, 400, 500, kEvict, 1, kKeepSeconds));
    CHECK(KeptByRebuild(ArenaKeep::kFrames, 400, 500, kEvict, 1, kKeepSeconds));
    CHECK_FALSE(KeptByRebuild(ArenaKeep::kFrame, 400, 500, kEvict, 1, kKeepSeconds));
    // one past the frames, within the seconds: by the first alone
    CHECK(KeptByRebuild(ArenaKeep::kKeep, 100, 500, kEvict, 4, kKeepSeconds));
    CHECK_FALSE(KeptByRebuild(ArenaKeep::kFrames, 100, 500, kEvict, 4, kKeepSeconds));
    // past both: by none (Evict would let it go too)
    CHECK_FALSE(KeptByRebuild(ArenaKeep::kKeep, 100, 5000, kEvict, 40, kKeepSeconds));
}
