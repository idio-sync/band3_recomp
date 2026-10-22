#pragma once

// VR's song HUD in pieces (vr_hud_split): in songs the HUD panel's picture
// (vr_pose.h's HudBoundary on, but for the track cameras' draws) is cut into
// the parts RB3 lays out at its edges, each shown as a quad layer of its own
// (a rectangle of the one HUD image: XrCompositionLayerQuad's
// subImage.imageRect, xr_present.cpp) at a depth that suits it. On one panel
// 1.8 m away the HUD sat where the 3D highway runs through (from 0.5 to 4.7
// m) with the venue behind it, and the eyes couldn't settle on it and the
// gems at once. Now the band meter stands beside the leftmost track and the
// score, stars and song progress beside the rightmost, at the strikeline's
// depth (or both above the tracks' middle where beside them would be far to
// the side: four parts'); the lyrics, the vocals' strip and the song's title
// above the game camera's window at its distance, or from a fixed viewpoint
// high ahead; anything else (`rest`) where the whole panel would be. Menus,
// a song's pause menu and a frame with a draw over the whole picture (a fade)
// keep the whole panel.
//
// The pieces are found from the HUD's draws (HudPiecesOf): each draw's
// rectangle of the game's picture (HudDrawRect), put in a piece by where it is
// (HudPieceOf: the left and right columns, the top), and held through the
// song (HudPieceHold: their union, so a piece only grows and what's in it
// stays put). PlaceHudPieces hangs them, each at the size the whole picture
// would be at vr_hud_piece_width degrees seen from the head and facing it.
// No OpenXR here: the same on every platform, and unit-tested.

#include <cmath>
#include <cstdint>

#include "src/Render/free_cam.h"
#include "src/Render/scene_capture.h"
#include "src/Render/soft_raster.h"
#include "src/Render/vr_pose.h"

namespace band3::render::vr {

// A rectangle of the game's picture, and so of the HUD's image (drawn at the
// eyes' size, the picture stretched to fill it): u across from its left, v
// down from its top, both 0..1
struct PictureRect {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0;
    bool Empty() const { return !(u1 > u0 && v1 > v0); }
    float Area() const { return Empty() ? 0 : (u1 - u0) * (v1 - v0); }
};

inline PictureRect Union(const PictureRect& a, const PictureRect& b) {
    if (a.Empty()) return b;
    if (b.Empty()) return a;
    return {std::fmin(a.u0, b.u0), std::fmin(a.v0, b.v0), std::fmax(a.u1, b.u1),
            std::fmax(a.v1, b.v1)};
}

// the band meter; the score, stars and song progress; the top strip (lyrics,
// the vocals' strip, the song's title); the rest
enum class HudPiece : uint8_t { kMeter, kScore, kTop, kRest };
inline constexpr int kHudPieces = 4;
// a quad layer each, over the eyes' projection layer and the dark's nine
// (vr_window_quads): within the 16 layers every OpenXR runtime takes
static_assert(1 + kFrameQuads + kHudPieces <= 16, "more layers than OpenXR promises");

inline const char* HudPieceName(HudPiece p) {
    switch (p) {
        case HudPiece::kMeter: return "meter";
        case HudPiece::kScore: return "score";
        case HudPiece::kTop: return "top";
        default: return "rest";
    }
}

// Where RB3 puts them, in the picture (1P, 2P and 4P alike: trk_a,
// screen-mp2-25s, screen-mp4-10s): the band meter at u 0.04..0.09, v
// 0.31..0.69; the score box, stars, progress bar and time at u 0.77..0.98
// (the progress bar's instrument icon the leftmost, its middle at 0.78),
// from v 0.20 (1P) or 0.25 (four parts, under the vocals' strip) down to
// 0.39; the song's title, artist, album and author right-aligned against it
// (ending at u 0.77, v 0.21..0.37) for the first seconds; the lyrics' lines
// at the top middle (v 0.08..0.14), or the vocals' strip across the top (u
// 0.03..0.97, v 0.04..0.31 with its Overdrive pills), and with two players
// or more a Unison Bonus's banner and icons under it (u 0.4..0.6, v
// 0.27..0.45); the pause menu low on player 1's side (u 0.08..0.42, v
// 0.53..0.99). A draw is the meter's wholly left of kMeterRight with its
// middle below kMeterTop; the score's wholly right of kScoreLeft, its middle
// right of kScoreMid (the title's lines end left of it however short) and
// between kScoreTop and kScoreBottom; the top's with its middle above
// kTopBottom; else the rest's.
inline constexpr float kMeterRight = 0.2f, kMeterTop = 0.3f;
inline constexpr float kScoreLeft = 0.72f, kScoreMid = 0.775f;
inline constexpr float kScoreTop = 0.15f, kScoreBottom = 0.6f;
inline constexpr float kTopBottom = 0.45f;
// around each draw's rectangle, for its edges' glow and the MSAA's (5
// pixels of 1280)
inline constexpr float kHudPad = 0.004f;
// a draw covering this much of the picture both ways is over the whole of it
// (the fade's quad, a pause menu's dimming)
inline constexpr float kHudFull = 0.9f;
// the rest's largest area for the HUD to be split: more is a menu over the
// song (the pause menu), shown on the whole panel
inline constexpr float kHudRestMax = 0.1f;

inline HudPiece HudPieceOf(const PictureRect& r) {
    const float mid = (r.v0 + r.v1) / 2;
    if (r.u1 <= kMeterRight && mid >= kMeterTop) return HudPiece::kMeter;
    if (r.u0 >= kScoreLeft && (r.u0 + r.u1) / 2 >= kScoreMid && mid >= kScoreTop &&
        mid <= kScoreBottom)
        return HudPiece::kScore;
    if (mid < kTopBottom) return HudPiece::kTop;
    return HudPiece::kRest;
}

// Back-buffer draw `d`'s rectangle of the picture: its vertices through its
// matrices and its camera's viewport (a DrawRect quad's rect), over the back
// buffer's size, padded by kHudPad and kept within the picture. False for a
// draw that shows nothing: blended away (Dest; SrcAlpha or SrcAlphaAdd at
// alpha 0, as the fade's quad is between fades), nothing in front of its
// camera, or no area (a point, as an empty text's mesh is)
inline bool HudDrawRect(const FrameCapture& fc, const DrawItem& d, PictureRect& out) {
    constexpr int kDest = 0, kSrcAlpha = 3, kSrcAlphaAdd = 4;
    if (d.blend == kDest || ((d.blend == kSrcAlpha || d.blend == kSrcAlphaAdd) &&
                             !(d.color[3] > 1.0f / 255)))
        return false;
    // the back buffer's size and the camera's viewport in it (the whole
    // picture without a camera, as old captures have it)
    float tw = 1280, th = 720;
    float vp[4] = {0, 0, tw, th};
    const CameraView* view = d.cam ? CameraOf(fc, d.cam) : nullptr;
    if (!view && !fc.cameras.empty()) view = &fc.cameras[0];
    if (view && view->target_w && view->target_h) {
        tw = float(view->target_w);
        th = float(view->target_h);
        vp[2] = tw;
        vp[3] = th;
        if (d.cam && view->cam == d.cam && view->viewport[2] > 0 && view->viewport[3] > 0)
            for (int k = 0; k < 4; k++) vp[k] = view->viewport[k];
    }
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    bool any = false;
    auto add = [&](float x, float y) {
        lo[0] = std::fmin(lo[0], x / tw);
        lo[1] = std::fmin(lo[1], y / th);
        hi[0] = std::fmax(hi[0], x / tw);
        hi[1] = std::fmax(hi[1], y / th);
        any = true;
    };
    if (d.rect_shader >= 0) {
        add(d.rect[0], d.rect[1]);
        add(d.rect[0] + d.rect[2], d.rect[1] + d.rect[3]);
    } else if (d.geom) {
        for (const Vertex& v : d.geom->verts) {
            float w[3], c[4];
            freecam::WorldPos(d, v, w);
            freecam::Transform(w, d.view_proj, c);
            if (!(c[3] > 0)) continue;
            add(vp[0] + (c[0] / c[3] + 1) / 2 * vp[2], vp[1] + (1 - c[1] / c[3]) / 2 * vp[3]);
        }
    }
    if (!any || !(hi[0] - lo[0] > 1e-4f) || !(hi[1] - lo[1] > 1e-4f)) return false;
    out.u0 = std::fmax(0.0f, lo[0] - kHudPad);
    out.v0 = std::fmax(0.0f, lo[1] - kHudPad);
    out.u1 = std::fmin(1.0f, hi[0] + kHudPad);
    out.v1 = std::fmin(1.0f, hi[1] + kHudPad);
    return !out.Empty();
}

// The HUD's pieces in one frame: each one's rectangle (the union of its
// draws'), its draws, and the point of the picture PlaceHudPieces hangs it
// by (`anchor`, u and v: the meter's bottom right corner, the score's bottom
// left, the top strip's bottom edge's middle of the picture, the rest's the
// picture's middle); and whether it can be split (`split`): no draw over the
// whole picture, none of a menu over the song (a camera of z range 0..1
// drawing after the flat HUD's, as the pause menu's does), the rest's at
// most kHudRestMax
struct HudPieces {
    bool split = false;
    PictureRect rect[kHudPieces];
    float anchor[kHudPieces][2] = {};
    uint32_t draws[kHudPieces] = {};
    uint32_t full = 0, menu = 0;  // draws over the whole picture, a menu's
};

// the point of `r` a piece of `kind` hangs by (HudPieces::anchor)
inline void HudAnchorOf(HudPiece kind, const PictureRect& r, float out[2]) {
    switch (kind) {
        case HudPiece::kMeter: out[0] = r.u1, out[1] = r.v1; break;
        case HudPiece::kScore: out[0] = r.u0, out[1] = r.v1; break;
        case HudPiece::kTop: out[0] = 0.5f, out[1] = r.v1; break;
        default: out[0] = out[1] = 0.5f; break;
    }
}

// The pieces of `fc`'s HUD panel as `o` draws it (BackBufferPart::kHud: its
// hud_from and eye_cams, soft_raster.h's OnHud): its back buffer's colour
// draws that show something. Not split for a panel that's the whole picture
// (hud_from 0: no world the eyes can see)
inline HudPieces HudPiecesOf(const FrameCapture& fc, const RasterOptions& o) {
    HudPieces p;
    const uint32_t from = HudFrom(o, fc);
    if (from == 0 || from == FrameCapture::kNoPost) return p;
    // the flat HUD's first draw: its camera's z range ends before 1
    size_t hud_first = fc.draws.size();
    for (size_t d = from; d < fc.draws.size() && hud_first == fc.draws.size(); d++) {
        const DrawItem& it = fc.draws[d];
        const CameraView* view = it.rect_shader < 0 && it.cam ? CameraOf(fc, it.cam) : nullptr;
        if (DrawnToBackBuffer(it) && view && view->zrange[1] < 1) hud_first = d;
    }
    for (size_t d = from; d < fc.draws.size(); d++) {
        const DrawItem& it = fc.draws[d];
        if (!DrawnToBackBuffer(it) || it.draw_mode != kDrawModeNormal || !OnHud(o, fc, d))
            continue;
        PictureRect r;
        if (!HudDrawRect(fc, it, r)) continue;
        if (r.u1 - r.u0 >= kHudFull && r.v1 - r.v0 >= kHudFull) {
            p.full++;
            continue;
        }
        const CameraView* view = it.rect_shader < 0 && it.cam ? CameraOf(fc, it.cam) : nullptr;
        if (d > hud_first && view && view->zrange[1] >= 1) p.menu++;
        const int k = int(HudPieceOf(r));
        p.rect[k] = Union(p.rect[k], r);
        p.draws[k]++;
    }
    for (int k = 0; k < kHudPieces; k++) HudAnchorOf(HudPiece(k), p.rect[k], p.anchor[k]);
    p.split =
        p.full == 0 && p.menu == 0 && p.rect[int(HudPiece::kRest)].Area() <= kHudRestMax;
    return p;
}

// a frame that can't be split shows the whole panel once this many in a row
// were (HudPieceHold), so a stray one doesn't flash it up
inline constexpr int kHudWholeAfter = 3;

// The pieces held through a song (native_view.cpp's VrFrame): each one the
// union of what it has been since the last Reset (the song's start), so a
// piece only grows and never jitters as lyrics' lines change length or the
// song's title goes; each hung by the anchor it first had, so what's in it
// stays where it was as it grows (a Unison Bonus under the vocals' strip
// reaches below where the strip hangs). Frames that can't be split (a pause
// menu) show the whole panel once kHudWholeAfter come in a row, until then
// the pieces as they were, and leave them as they were.
class HudPieceHold {
 public:
    HudPieces Hold(const HudPieces& now) {
        if (!now.split) {
            if (++unsplit_ < kHudWholeAfter && held_.split) return held_;
            return now;
        }
        unsplit_ = 0;
        for (int k = 0; k < kHudPieces; k++) {
            if (held_.rect[k].Empty())
                for (int c = 0; c < 2; c++) held_.anchor[k][c] = now.anchor[k][c];
            held_.rect[k] = Union(held_.rect[k], now.rect[k]);
            held_.draws[k] = now.draws[k];
        }
        held_.split = true;
        held_.full = held_.menu = 0;
        return held_;
    }
    void Reset() {
        held_ = {};
        unsplit_ = 0;
    }

 private:
    HudPieces held_;
    int unsplit_ = 0;
};

// How PlaceHudPieces hangs them (the settings)
struct HudSplitLayout {
    bool on = true;  // vr_hud_split
    // vr_hud_piece_width: each piece is the size it'd be in the whole
    // picture seen this many degrees across from the head (vr_hud_width's
    // panel: 55), at whatever distance it hangs
    float piece_width = 55;
    // vr_hud_side_gap: metres between a track's edge (at its strikeline)
    // and the meter or score beside it
    float side_gap = 0.05f;
    // vr_hud_side_max: the meter and score go beside the outermost tracks
    // only while their middles are within this many degrees of straight
    // ahead (one player's: 17 and 25), else above the tracks' middle (two
    // players' would be 38 and 41, four parts' 47 and 52)
    float side_max = 40;
    // vr_lyrics_height: through the game camera's window, metres from the
    // window's top edge up to the top strip's bottom (negative: down into it)
    float lyrics_height = 0.05f;
    // vr_lyrics_distance, vr_lyrics_elevation: from a fixed viewpoint, the
    // top strip's bottom middle this many metres ahead along the floor and
    // this many degrees above the eyes
    float lyrics_distance = 2.5f;
    float lyrics_elevation = 15;
};

struct HudPieceQuad {
    HudPiece kind = HudPiece::kRest;
    PictureRect rect;  // of the HUD's image
    HudQuad quad;      // where it hangs in LOCAL space, facing the head
};

// the frame's pieces: none (count 0) shows the whole panel (FrameLayout::
// hud_quad); `beside`: the meter and score beside the outermost tracks, else
// above their middle
struct HudSplit {
    int count = 0;
    bool beside = false;
    HudPieceQuad piece[kHudPieces];
};

// half a track's width and the point along it where the game's picture fades
// it out, in its units (TrackToLocal's)
inline constexpr float kTrackHalfWidth = 9.8f;
inline constexpr float kTrackFar = 106;
// the meter and score above the tracks' middle clear the tracks' far ends by
// this many metres, seen from the head
inline constexpr float kAboveTracks = 0.03f;

// A quad's turn that faces `from` (its +z toward it) from `at`, level: a yaw,
// then a pitch toward it (YawPitch)
inline void FacingFrom(const float at[3], const float from[3], float q[4]) {
    const float f[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    const float flat = std::sqrt(f[0] * f[0] + f[2] * f[2]);
    YawPitch(std::atan2(-f[0], -f[2]), std::atan2(f[1], flat), q);
}

// Piece `r` of the picture on a plane through `at` turned `q`, where the
// picture's point (au, av) is, `sx` and `sy` metres the picture's width and
// height
inline HudPieceQuad PieceOnPlane(HudPiece kind, const PictureRect& r, float au, float av,
                                 const float at[3], const float q[4], float sx, float sy) {
    HudPieceQuad p;
    p.kind = kind;
    p.rect = r;
    const float off[3] = {((r.u0 + r.u1) / 2 - au) * sx, -((r.v0 + r.v1) / 2 - av) * sy, 0};
    float turned[3];
    Rotate(q, off, turned);
    for (int k = 0; k < 3; k++) {
        p.quad.pose.p[k] = at[k] + turned[k];
        p.quad.pose.q[k] = q[k];
    }
    p.quad.pose.q[3] = q[3];
    p.quad.size[0] = (r.u1 - r.u0) * sx;
    p.quad.size[1] = (r.v1 - r.v0) * sy;
    return p;
}

// Piece `r` facing the head (`from`) with its picture's point (au, av) at
// `at`, at the size the whole picture would be `width_deg` across from there
inline HudPieceQuad PieceFacing(HudPiece kind, const PictureRect& r, float au, float av,
                                const float at[3], const float from[3], float width_deg) {
    const float d[3] = {at[0] - from[0], at[1] - from[1], at[2] - from[2]};
    const float sx = 2 * freecam::Len3(d) * std::tan(width_deg * 3.14159265f / 360);
    float q[4];
    FacingFrom(at, from, q);
    return PieceOnPlane(kind, r, au, av, at, q, sx, sx / kWindowAspect);
}

// The frame's pieces (VrFrame's, the harness's capture), from `p` (held) and
// `l`, ResolveFrame's: in songs with the tracks in front (track_count) and
// the panel on its own place, with `s.on`; else none, the whole panel. `head`
// is the recenter's (FrameInputs::head), its yaw the tracks' and window's.
// Each piece is hung by its anchor (HudPieces::anchor, as first seen):
//   meter: its bottom right corner `side_gap` left of the leftmost track's
//     left edge at its strikeline, at the strikeline's height
//   score: its bottom left corner `side_gap` right of the rightmost track's
//     right edge likewise
//   or, where either's middle would be more than `side_max` degrees from
//     straight ahead, both above the tracks' middle (halfway between the
//     outermost edges, as deep as the strikeline nearest it), `side_gap`
//     apart either side of it, their bottoms clear of every track's far end
//     seen from the head
//   top: the picture's middle (u 0.5) at its bottom edge `lyrics_height`
//     above the window's top edge's middle, or from a fixed viewpoint
//     `lyrics_distance` ahead and `lyrics_elevation` degrees up
//   rest: where it is on the whole panel (hud_quad), at its size
// each facing the head, at the size the whole picture would be
// `piece_width` degrees across from it (rest: the panel's)
inline HudSplit PlaceHudPieces(const FrameLayout& l, const XrPose& head, const HudPieces& p,
                               const HudSplitLayout& s) {
    HudSplit out;
    if (!s.on || !l.hud || l.hud_in_window || l.eyes_black || l.track_count <= 0 || !p.split)
        return out;
    const float yaw = HeadYaw(head);
    const float right[3] = {std::cos(yaw), 0, -std::sin(yaw)};
    const float fwd[3] = {-std::sin(yaw), 0, -std::cos(yaw)};
    auto rel = [&](const float x[3], float out3[3]) {
        for (int k = 0; k < 3; k++) out3[k] = x[k] - head.p[k];
    };
    auto across = [&](const float x[3]) {
        float r[3];
        rel(x, r);
        return freecam::Dot3(r, right);
    };
    auto ahead = [&](const float x[3]) {
        float r[3];
        rel(x, r);
        return freecam::Dot3(r, fwd);
    };
    // the tracks' outermost edges at their strikelines, their strikelines,
    // and how high their far ends are seen from the head
    float left_edge[3] = {}, right_edge[3] = {}, strike[kMaxTrackCams][3] = {};
    float least = 1e30f, most = -1e30f, far_up = -1e30f;
    for (int t = 0; t < l.track_count; t++) {
        const float pts[4][3] = {
            {-kTrackHalfWidth, 0, 0}, {kTrackHalfWidth, 0, 0}, {0, 0, 0}, {0, kTrackFar, 0}};
        float loc[4][3];
        for (int k = 0; k < 4; k++) TrackToLocal(l.tracks[t], l.track_scale, pts[k], loc[k]);
        for (int k = 0; k < 2; k++) {
            const float a = across(loc[k]);
            if (a < least) {
                least = a;
                for (int c = 0; c < 3; c++) left_edge[c] = loc[k][c];
            }
            if (a > most) {
                most = a;
                for (int c = 0; c < 3; c++) right_edge[c] = loc[k][c];
            }
        }
        for (int c = 0; c < 3; c++) strike[t][c] = loc[2][c];
        float r[3];
        rel(loc[3], r);
        far_up = std::fmax(far_up, std::atan2(r[1], std::sqrt(r[0] * r[0] + r[2] * r[2])));
    }
    // piece `kind` with its anchor at `at`
    auto piece = [&](HudPiece kind, const float at[3]) {
        const float* a = p.anchor[int(kind)];
        return PieceFacing(kind, p.rect[int(kind)], a[0], a[1], at, head.p, s.piece_width);
    };
    // its middle's angle from straight ahead
    auto angle = [&](HudPiece kind, const float at[3]) {
        if (p.rect[int(kind)].Empty()) return 0.0f;
        const HudPieceQuad q = piece(kind, at);
        return std::fabs(std::atan2(across(q.quad.pose.p), ahead(q.quad.pose.p))) * 57.29578f;
    };
    // beside the outermost tracks
    float meter_at[3], score_at[3];
    for (int c = 0; c < 3; c++) {
        meter_at[c] = left_edge[c] - right[c] * s.side_gap;
        score_at[c] = right_edge[c] + right[c] * s.side_gap;
    }
    out.beside = angle(HudPiece::kMeter, meter_at) <= s.side_max &&
                 angle(HudPiece::kScore, score_at) <= s.side_max;
    if (!out.beside) {
        // above the tracks' middle (halfway between the outermost edges) at
        // the depth of the strikeline nearest it, clear of the far ends
        const float mid = (least + most) / 2;
        int closest = 0;
        for (int t = 1; t < l.track_count; t++)
            if (std::fabs(across(strike[t]) - mid) < std::fabs(across(strike[closest]) - mid))
                closest = t;
        const float depth = ahead(strike[closest]);
        const float up = head.p[1] + depth * std::tan(far_up) + kAboveTracks;
        for (int c = 0; c < 3; c++) {
            const float at = head.p[c] + fwd[c] * depth + right[c] * mid;
            meter_at[c] = at - right[c] * s.side_gap / 2;
            score_at[c] = at + right[c] * s.side_gap / 2;
        }
        meter_at[1] = score_at[1] = std::fmax(up, strike[closest][1]);
    }
    if (!p.rect[int(HudPiece::kMeter)].Empty())
        out.piece[out.count++] = piece(HudPiece::kMeter, meter_at);
    if (!p.rect[int(HudPiece::kScore)].Empty())
        out.piece[out.count++] = piece(HudPiece::kScore, score_at);
    if (!p.rect[int(HudPiece::kTop)].Empty()) {
        float at[3];
        if (l.window) {
            const float edge[3] = {0, l.window_frame.size[1] / 2 + s.lyrics_height, 0};
            WindowToLocal(l.window_frame, edge, at);
        } else {
            const float up = s.lyrics_distance * std::tan(s.lyrics_elevation * 3.14159265f / 180);
            for (int c = 0; c < 3; c++) at[c] = head.p[c] + fwd[c] * s.lyrics_distance;
            at[1] += up;
        }
        out.piece[out.count++] = piece(HudPiece::kTop, at);
    }
    const PictureRect& rest = p.rect[int(HudPiece::kRest)];
    if (!rest.Empty()) {
        const float* a = p.anchor[int(HudPiece::kRest)];
        out.piece[out.count++] =
            PieceOnPlane(HudPiece::kRest, rest, a[0], a[1], l.hud_quad.pose.p, l.hud_quad.pose.q,
                         l.hud_quad.size[0], l.hud_quad.size[1]);
    }
    return out;
}

// a piece's rectangle of an image `w` by `h` texels: whole texels, outward
inline void PieceTexels(const PictureRect& r, uint32_t w, uint32_t h, int32_t& x, int32_t& y,
                        int32_t& rw, int32_t& rh) {
    const auto clamp = [](float v, uint32_t n) {
        return int32_t(v < 0 ? 0 : v > float(n) ? float(n) : v);
    };
    x = clamp(std::floor(r.u0 * float(w)), w);
    y = clamp(std::floor(r.v0 * float(h)), h);
    rw = clamp(std::ceil(r.u1 * float(w)), w) - x;
    rh = clamp(std::ceil(r.v1 * float(h)), h) - y;
}

}  // namespace band3::render::vr
