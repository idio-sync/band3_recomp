#pragma once

// VR's fixed viewpoints (vr_viewpoint front_row, on_stage, venue): where they
// are in whichever venue a song is in. RB3's venues share no coordinates (the
// stage, the band and the crowd stand somewhere else in each), so each place
// is found from what the game drew: the band members and the crowds, whose
// meshes the capture knows (FrameCapture::mesh_owners, marked by
// src/Hooks/vr_view.cpp around BandCharacter::DrawLodOrShadow and
// WorldCrowd::DrawShowing). With a fixed viewpoint in a song every shot
// draws the whole band and every crowd (vr_view.cpp), so one frame has them
// all. No OpenXR here, and unit-tested.
//
// Venue units are Milo's inches (kUnitsPerMetre), x right, y forward, z up.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

#include "src/Render/scene_capture.h"
#include "src/Render/vr_pose.h"

namespace band3::render::vr {

inline constexpr int kMaxMembers = 8;

// The venue as the band and the crowd lay it out (VenueRefOf). Each member
// is where it stands: the middle of its drawn mesh across the floor, its
// lowest point (its feet: the stage floor) up. `toward` is along the floor
// from the band to the audience: toward the crowd's middle (`toward_crowd`),
// or -y without a crowd (the venues' own habit: arena_04's audience is on
// -y). Depth is along `toward` from `centre`, across along `across` (toward
// turned a quarter counterclockwise).
struct VenueRef {
    int members = 0;
    uint32_t owner[kMaxMembers] = {};
    float member[kMaxMembers][3] = {};
    // the members' middle, at the stage floor (the members' median feet)
    float centre[3] = {0, 0, 0};
    float toward[2] = {0, -1};
    float across[2] = {1, 0};
    bool toward_crowd = false;
    // the band's depth (front: nearest the audience) and half width
    float band_front = 0, band_back = 0, band_half = 0;
    // crowd points (a card, or a 3D crowd member's mesh) and those in front
    // of the band, within the band's width and 4 m either side, the last
    // row's depth (90th percentile); of those 2 m either side of the band's
    // middle (or all of them, without three there), the first row's depth
    // (2nd percentile) and the audience's floor (their median lowest point,
    // at most the stage floor)
    int crowd = 0;
    int crowd_ahead = 0;
    float crowd_front = 0, crowd_back = 0, audience_floor = 0;
    bool Valid() const { return members > 0; }
};

// `fc`'s world camera's draws of the band and the crowds (mesh_owners); a
// VenueRef without members if the band isn't in it
VenueRef VenueRefOf(const FrameCapture& fc);

// Moves a fixed viewpoint from where VenueRefOf puts it (vr_viewpoint_offset):
// metres to its right, ahead and up, and degrees turned counterclockwise
// seen from above
struct ViewOffset {
    float right = 0, ahead = 0, up = 0, yaw_deg = 0;
};

// "right,ahead,up[,yaw]"; false if it isn't three or four numbers
inline bool ParseViewOffset(const std::string& text, ViewOffset& o) {
    float v[4] = {0, 0, 0, 0};
    const char* at = text.c_str();
    int n = 0;
    while (n < 4) {
        char* end = nullptr;
        v[n] = std::strtof(at, &end);
        if (end == at) return false;
        n++;
        while (*end == ' ') end++;
        if (!*end) {
            at = end;
            break;
        }
        if (*end != ',') return false;
        at = end + 1;
    }
    if (*at || n < 3) return false;
    o = {v[0], v[1], v[2], v[3]};
    return true;
}

// The eye's height above the floor it stands on, metres
inline constexpr float kEyeHeight = 1.7f;

// Where a fixed viewpoint puts LOCAL space's origin (the head at the
// session's start) in `r`'s venue, `units` venue units a metre (39.37 times
// vr_world_scale) for the eye's height (a bigger venue scale is a taller
// viewer); distances on the floor are the venue's own (kUnitsPerMetre):
//   front_row  at the barrier: on the band's middle line, 0.3 m in front of
//              the crowd's first row (5 m past the front-most member without
//              a crowd, and never under 2 m), eyes kEyeHeight above the
//              audience's floor (1.2 m under the stage's without a crowd),
//              facing the band
//   on_stage   among the band, facing the audience, eyes kEyeHeight above the
//              stage floor: of the places on the stage 0.25 m apart between
//              the back-most member and 0.5 m past the front-most, at least
//              0.9 m from every member and with none of them between it and
//              the audience, the one nearest 0.5 m downstage of the band's
//              front line (the members' median depth), then nearest its
//              middle line, clear of them by up to 1.2 m
//   venue      behind and above the audience: 2 m behind the crowd's last row
//              in front (15 m past the front-most member without one), raised
//              0.35 of its distance from the band above the stage floor (2.5
//              to 7 m), facing the band
// then moved by `off`. False for game_camera or without members.
bool PresetAnchor(ViewMode mode, const VenueRef& r, const ViewOffset& off, float units,
                  Anchor& out);

// The horizontal distance in venue units within which crowd draws are left
// out of a fixed viewpoint's eyes (Eye::crowd_clear): vr_crowd_clear metres
inline float CrowdClear(ViewMode mode, float metres) {
    return mode == ViewMode::kGameCamera || !(metres > 0) ? 0 : metres * kUnitsPerMetre;
}

inline const char* ViewModeName(ViewMode m) {
    switch (m) {
    case ViewMode::kFrontRow: return "front_row";
    case ViewMode::kOnStage: return "on_stage";
    case ViewMode::kVenue: return "venue";
    case ViewMode::kGameCamera: break;
    }
    return "game_camera";
}

// The reference a song's fixed viewpoints use (xr_present.cpp's LayoutFor):
// taken from the song's frames as they come, kept for the song, and taken
// again only from a frame with more of the band than it had (a member hidden
// as the song began) or the crowd it lacked; forgotten when the song ends,
// so the next song's venue is found afresh. `Next` returns whether the
// reference changed.
class VenueTracker {
 public:
    bool Next(const FrameCapture& fc, bool in_song) {
        if (!in_song) {
            in_song_ = false;
            return false;
        }
        if (!in_song_) {
            in_song_ = true;
            ref_ = {};
            tried_band_ = 0;
            tried_crowd_ = false;
        }
        // what the frame has, before working it out (once for each more
        // member, and once for a first crowd)
        int band = 0;
        bool crowd = false;
        uint32_t seen[kMaxMembers] = {};
        for (const MeshOwner& o : fc.mesh_owners) {
            crowd |= o.kind == kOwnerCrowd;
            if (o.kind != kOwnerBand) continue;
            bool known = false;
            for (int i = 0; i < band; i++) known |= seen[i] == o.owner;
            if (!known && band < kMaxMembers) seen[band++] = o.owner;
        }
        const bool more_band = band > tried_band_;
        const bool first_crowd = crowd && !tried_crowd_ && band > 0;
        if (!more_band && !first_crowd) return false;
        tried_band_ = std::max(tried_band_, band);
        tried_crowd_ |= crowd && band > 0;
        const VenueRef r = VenueRefOf(fc);
        const bool better = r.members > ref_.members ||
                            (r.members == ref_.members && r.crowd > 0 && ref_.crowd == 0);
        if (!r.Valid() || !better) return false;
        ref_ = r;
        return true;
    }
    const VenueRef& Ref() const { return ref_; }

 private:
    bool in_song_ = false;
    VenueRef ref_;
    int tried_band_ = 0;
    bool tried_crowd_ = false;
};

}  // namespace band3::render::vr
