#pragma once

// vr_hold_band's decisions (src/Hooks/vr_view.cpp), apart from the game so
// they're unit-tested.
//
// RB3 moves the band at every camera cut, twice over:
//   - BandCamShot::StartAnim (rb3-xenon bandobj/BandCamShot.cpp) keeps each
//     of the shot's targets' local transform and moves it onto the shot's
//     mark (TeleportTarget(t, mXfm, false)), and EndAnim puts it back where it
//     was kept (TeleportTarget(t, kept, true), every target with mReturn, all
//     by default). Every target is a band member, and a mark is often a metre
//     or more from where the member stood, turned tens of degrees.
//   - BandWardrobe::StartVenueShot flags every member to go home at its next
//     BandCharacter::Poll: Teleport() onto its waypoint (the place the venue
//     gives it on the stage), undoing the walking and turning its clips did
//     during the shot (up to a metre or so, and as much as a half turn).
// Seen from a fixed viewpoint, either reads as the band jumping about.
//
// Held, the shots' moves onto their marks and back are skipped
// (TeleportGate), and so is the move home; instead a leash (LeashStep) keeps
// each member near home a little at a time: free within kLeashRadius and
// kLeashTurn of its waypoint, pulled back from beyond over kLeashSeconds, and
// harder past kLeashFarRadius or kLeashFarTurn.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace band3::vr_view {

// Which of BandCamShot::TeleportTarget's moves go ahead. A member a shot moved
// onto its mark before the hold began (the viewpoint chosen mid-shot) is
// still put back as that shot ends, so it isn't left on the mark; the rest of
// a held song's moves, onto marks and back, are skipped (there is nothing to
// put back).
class TeleportGate {
 public:
    // TeleportTarget(`target`, ..., `back`): whether it goes ahead, `hold`
    // being whether the band is held now
    bool Allow(uint32_t target, bool back, bool hold) {
        auto at = std::find(away_.begin(), away_.end(), target);
        if (back) {
            if (at != away_.end()) {
                away_.erase(at);
                return true;
            }
            return !hold;
        }
        if (hold) return false;
        if (at == away_.end()) away_.push_back(target);
        return true;
    }

    // the song is over: its characters go, and their addresses may come back
    // as others'
    void Reset() { away_.clear(); }

    // members a shot moved onto its mark and hasn't put back
    size_t Away() const { return away_.size(); }

 private:
    std::vector<uint32_t> away_;
};

// The leash, in metres, degrees and seconds
inline constexpr float kLeashRadius = 0.5f, kLeashTurn = 60;
inline constexpr float kLeashFarRadius = 1.25f, kLeashFarTurn = 120;
inline constexpr float kLeashSeconds = 1.5f, kLeashFarSeconds = 0.4f;

// How far `off` (a distance or a turn from home) is after `dt` seconds on the
// leash: what's past `free` shrinks by dt / kLeashSeconds of itself, and what's
// past `far` by dt / kLeashFarSeconds, each all of it at most; nothing
// within `free`
inline float Leashed(float off, float free, float far, float dt) {
    if (off <= free) return off;
    const float past_far = std::max(0.0f, off - far);
    const float between = off - free - past_far;
    return free + between * (1 - std::min(1.0f, dt / kLeashSeconds)) +
           past_far * (1 - std::min(1.0f, dt / kLeashFarSeconds));
}

// `now` (x, y across the floor in `units` a metre, and heading in degrees,
// counterclockwise) moved toward `home` for `dt` seconds (Leashed): its
// distance across the floor along the line home, free to kLeashRadius, taken
// back over kLeashSeconds past it and over kLeashFarSeconds past
// kLeashFarRadius; its heading likewise past kLeashTurn and kLeashFarTurn,
// the short way round. Nothing is ever put back at once, so the leash never
// makes a jump of its own: a member walking away at 1 m/s settles about 1.45 m
// out. Returns whether it moved.
inline bool LeashStep(const float home[3], float now[3], float dt, float units) {
    if (!(dt > 0)) return false;
    bool moved = false;
    const float d[2] = {now[0] - home[0], now[1] - home[1]};
    const float dist = std::hypot(d[0], d[1]);
    const float to = Leashed(dist, kLeashRadius * units, kLeashFarRadius * units, dt);
    if (to < dist) {
        now[0] = home[0] + d[0] * to / dist;
        now[1] = home[1] + d[1] * to / dist;
        moved = true;
    }
    const float turn = std::remainder(now[2] - home[2], 360.0f);
    const float off = std::fabs(turn);
    const float turned = Leashed(off, kLeashTurn, kLeashFarTurn, dt);
    if (turned < off) {
        now[2] = home[2] + std::copysign(turned, turn);
        moved = true;
    }
    return moved;
}

}  // namespace band3::vr_view
