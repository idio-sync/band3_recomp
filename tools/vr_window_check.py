"""vr_window_check: VR's window onto the game camera against the game's own picture.

With `vr` on, the harness's `capture <name>` draws the captured frame as the
headset's eyes see it (src/Test/test_server.cpp): <name>.gpu.L.png and
.gpu.R.png, the frame's own camera with only the eyes' draws (those before
vr_pose.h's HudBoundary) at the game's size, <name>.gpu.eyes.png, the HUD
panel's picture, <name>.gpu.hud.png, and the layout, <name>.vr.json: whether
the eyes look through the game camera's window or are black, the window's
corners in the left eye's picture (`window_px`), the panel's (`hud_px`) and
where it goes (`hud`: window, own, off), the dark's alpha (`dim`), the fade,
and the window's width (`window_deg`, vr_window_width) and how much the game's
picture is magnified to fill it (`magnify`, vr_pose.h's WindowMagnify). In songs
a narrow shot's squeeze is capped (vr_window_max_squeeze): the picture then
fills only the window's centre, `picture_scale` of it each way, its corners
`picture_px`, the dark around it.
Without a VR session the eyes are a stand-in's: the head at LOCAL space's
origin, both eyes at it, 60 degrees each way, so the left eye sees the window
square on from where the game camera is. With a window the capture adds such
an eye at the recenter's head with the venue alone (no tracks),
<name>.gpu.C.png (the layout's `centre`), and the window is checked in that.
In songs the eyes have the tracks in front (vr_track_3d), and `tracks` has
each one's quad in the left eye's picture and in the panel's.

For each name this:

  window     takes the picture's quad (the window's, or with a capped squeeze
             its centre: `picture_px`) out of .gpu.L.png (or .gpu.C.png),
             upright whichever way the eye was turned, and sets it against
             .gpu.eyes.png box-filtered to the quad's size: mean difference,
             % of pixels off by more than 16 in their largest channel, and the
             worst cell of a 4x4 grid. Seen from the head at the recenter the
             window is the game camera's picture whatever its size, so these
             are small (post effects drawn
             at another size differ a little); `shifted` is the quad moved a
             tenth of its width, what a misplaced window would score.
             <name>.vr.window.png has the two side by side
  black      with `eyes_black` (no world the eyes can see), that both eye
             pictures are black, as the headset's are
  anchor     from a fixed viewpoint (vr_viewpoint front_row, on_stage, venue: no
             window), where the eyes are (`anchor`) and the song's venue as it
             was found (`venue`: the band, the stage floor, the crowd); with
             any eyes, the left eye's cost (`eye_cost`)
  tracks     that each track is in the eyes and gone from the panel: the
             panel's mean alpha inside the quad the track had in the game's
             picture (at most 0.15), and with the stand-in the left eye
             against the venue alone (.gpu.C.png) inside the track's quad,
             over 4 times what they differ by elsewhere in the window, plus 5
  sim        writes <name>.vr.sim.png: the left eye as the headset composes it,
             the eyes faded by `fade`, the dark at `dim` around the window
             (with `frame_layers`, vr_window_quads; else the eyes drew it),
             and the panel's picture (premultiplied) over it where `hud_px`
             says; or in songs with vr_hud_split its pieces (`hud_split`:
             each one's rectangle of the picture where its `px` says, listed
             with where it hangs from the head), and <name>.vr.pieces.png,
             the same with each piece's quad outlined

It exits 1 when a window's mean is over --max-mean (6), a black eye isn't, or
a track check fails.

  python tools/vr_window_check.py <screenshots>/hub <screenshots>/song
  python tools/vr_window_check.py <screenshots>/hub --max-mean 4
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw


def load(path):
    return np.asarray(Image.open(path).convert("RGBA"), dtype=np.float32)


def box_resize(img, w, h):
    return np.asarray(Image.fromarray(img.astype(np.uint8)).resize((w, h), Image.BOX),
                      dtype=np.float32)


def compare(a, b):
    """mean |a-b| over rgb, % of pixels off by more than 16, worst 4x4 cell mean"""
    d = np.abs(a[..., :3] - b[..., :3])
    big = d.max(axis=2) > 16
    h, w = d.shape[:2]
    worst = 0.0
    for gy in range(4):
        for gx in range(4):
            cell = d[gy * h // 4:(gy + 1) * h // 4, gx * w // 4:(gx + 1) * w // 4]
            if cell.size:
                worst = max(worst, float(cell.mean()))
    return float(d.mean()), float(big.mean() * 100), worst


def perspective_coeffs(dst, src):
    """PIL's PERSPECTIVE data mapping output points `dst` to input points `src`"""
    m, v = [], []
    for (x, y), (u, w) in zip(dst, src):
        m.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        m.append([0, 0, 0, x, y, 1, -w * x, -w * y])
        v += [u, w]
    return np.linalg.solve(np.array(m, dtype=np.float64), np.array(v, dtype=np.float64))


def window_crop(img, corners, w, h):
    """the quad `corners` (top left, top right, bottom right, bottom left) of
    `img` as a w x h rectangle, upright whichever way the eye was turned"""
    coeffs = perspective_coeffs([(0, 0), (w, 0), (w, h), (0, h)], corners)
    return np.asarray(Image.fromarray(img.astype(np.uint8)).transform(
        (w, h), Image.PERSPECTIVE, tuple(coeffs), Image.BILINEAR), dtype=np.float32)


def window_mask(shape, corners):
    """1 inside the quad `corners` (convex, in order), 0 outside"""
    h, w = shape[:2]
    ys, xs = np.mgrid[0:h, 0:w]
    px, py = xs + 0.5, ys + 0.5
    left = np.ones((h, w), dtype=bool)
    right = np.ones((h, w), dtype=bool)
    for i in range(4):
        (x0, y0), (x1, y1) = corners[i], corners[(i + 1) % 4]
        side = (x1 - x0) * (py - y0) - (y1 - y0) * (px - x0)
        left &= side >= 0
        right &= side <= 0
    return left | right


def texels(rect, w, h):
    """a piece's rectangle (u0, v0, u1, v1) of a w x h image in whole texels,
    outward, as vr_hud.h's PieceTexels"""
    x0, y0 = int(np.floor(rect[0] * w)), int(np.floor(rect[1] * h))
    x1, y1 = int(np.ceil(rect[2] * w)), int(np.ceil(rect[3] * h))
    return max(0, x0), max(0, y0), min(w, x1), min(h, y1)


def over(out, hud, box, corners):
    """the rectangle `box` (x0, y0, x1, y1) of the panel's picture `hud`
    (premultiplied) put over `out` on the quad `corners`, as a quad layer"""
    x0, y0, x1, y1 = box
    coeffs = perspective_coeffs(corners, [(x0, y0), (x1, y0), (x1, y1), (x0, y1)])
    h, w = out.shape[:2]
    warped = np.asarray(hud.transform((w, h), Image.PERSPECTIVE, tuple(coeffs), Image.BILINEAR),
                        dtype=np.float32)
    # outside the quad the transform samples nothing: transparent
    warped = warped * window_mask(out.shape, corners)[..., None]
    a = warped[..., 3:4] / 255
    return warped[..., :3] + out * (1 - a)


def simulate(prefix, layout, left):
    """the left eye as the compositor shows it: fade, dark, panel or its pieces
    (vr_hud_split), each outlined in <name>.vr.pieces.png"""
    out = left[..., :3] * (1 - layout["fade"])
    if layout["eyes_black"]:
        out[:] = 0
    # the layout's pixels are the headset's image's (`eye_size`); the eye's
    # picture is drawn smaller from a fixed viewpoint (vr_eye_scale)
    scale = (out.shape[1] / layout["eye_size"][0], out.shape[0] / layout["eye_size"][1])

    def scaled(corners):
        return [None if c is None else [c[0] * scale[0], c[1] * scale[1]] for c in corners]

    # the dark as quad layers (vr_window_quads); else the eyes drew it
    if layout["window"] and layout["frame_quads"] > 0 and layout.get("frame_layers", True):
        inside = window_mask(out.shape, scaled(layout.get("picture_px") or layout["window_px"]))
        out[~inside] *= 1 - layout["dim"]
    hud_path = prefix + ".gpu.hud.png"
    corners = scaled(layout.get("hud_px") or [])
    split = layout.get("hud_split") or {"count": 0, "pieces": []}
    shown = []
    if layout["hud"] != "off" and os.path.exists(hud_path):
        hud = Image.open(hud_path).convert("RGBA")
        if split["count"] > 0:
            for p in split["pieces"]:
                px = scaled(p["px"])
                if None in px:
                    continue
                out = over(out, hud, texels(p.get("image", p["rect"]), hud.width, hud.height), px)
                shown.append(px)
        elif None not in corners:
            out = over(out, hud, (0, 0, hud.width, hud.height), corners)
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(prefix + ".vr.sim.png")
    if shown:
        # each piece's quad outlined, to see where they hang
        img = Image.fromarray(np.clip(out, 0, 255).astype(np.uint8))
        draw = ImageDraw.Draw(img)
        for c in shown:
            draw.polygon([tuple(x) for x in c], outline=(255, 0, 255))
        img.save(prefix + ".vr.pieces.png")


def split_report(layout):
    """the panel's pieces (vr_hud_split): each one's rectangle of the picture
    and where it hangs"""
    split = layout.get("hud_split")
    if not split or not split["count"]:
        print("  hud split: none (the whole panel)")
        return
    print(f"  hud split: {split['count']} pieces, the meter and score "
          f"{'beside the tracks' if split['beside'] else 'above their middle'}")
    for p in split["pieces"]:
        f = p["from_head"]
        away = float(np.linalg.norm(f))
        print(f"    {p['kind']}: u {p['rect'][0]:.3f}..{p['rect'][2]:.3f} v {p['rect'][1]:.3f}.."
              f"{p['rect'][3]:.3f}, {p['size'][0]:.3f} x {p['size'][1]:.3f} m, {f[0]:.2f} m ahead, "
              f"{f[1]:.2f} below, {f[2]:.2f} across ({away:.2f} m away, "
              f"{np.degrees(2 * np.arctan2(p['size'][0] / 2, away)):.1f} degrees wide)")


def track_check(prefix, layout):
    """with the tracks in the eyes (vr_track_3d): each track's quad (from where
    the game's picture cuts it below to 60 units along it) in the left eye
    against the centre eye, which has the venue alone (the stand-in's left eye
    is where the centre eye is, so they differ only by the tracks there), and
    outside it in the window; and the panel's alpha where the track was in the
    game's picture"""
    tracks = layout.get("tracks") or []
    if not tracks:
        print("  tracks: none")
        return True
    ok = True
    hud_path = prefix + ".gpu.hud.png"
    hud = load(hud_path) if os.path.exists(hud_path) else None
    same_eye = layout["source"] == "stand_in" and "centre" in layout and os.path.exists(
        prefix + ".gpu.C.png")
    if same_eye:
        left, centre = load(prefix + ".gpu.L.png"), load(prefix + ".gpu.C.png")
        same_eye = left.shape == centre.shape
    if same_eye:
        diff = np.abs(left[..., :3] - centre[..., :3]).max(axis=2)
        anywhere = np.zeros(diff.shape, dtype=bool)
        for t in tracks:
            if None not in t["eye_px"][:4]:
                anywhere |= window_mask(diff.shape, t["eye_px"][:4])
        outside = ~anywhere
        picture = layout.get("picture_px") or layout["window_px"]
        if layout["window"] and None not in picture:
            outside &= window_mask(diff.shape, picture)
        out_mean = float(diff[outside].mean()) if outside.any() else 0.0
    for t in tracks:
        line = f"  track {t['cam']}:"
        if hud is not None:
            inside = window_mask(hud.shape, t["panel_px"][:4])
            alpha = float(hud[..., 3][inside].mean() / 255) if inside.any() else 0.0
            good = alpha <= 0.15
            ok &= good
            line += f" panel alpha where it was {alpha:.3f} {'ok' if good else 'NOT CLEAR'};"
        if same_eye and None not in t["eye_px"][:4]:
            inside = window_mask(diff.shape, t["eye_px"][:4])
            in_mean = float(diff[inside].mean()) if inside.any() else 0.0
            good = in_mean > 4 * out_mean + 5
            ok &= good
            line += (f" left eye against the venue alone: {in_mean:.1f} on it, {out_mean:.1f} "
                     f"elsewhere in the window {'ok' if good else 'NOT THERE'}")
        elif not same_eye:
            line += " (the eyes against the venue alone: with the stand-in only)"
        print(line)
    return ok


def check(prefix, max_mean):
    name = os.path.basename(prefix)
    with open(prefix + ".vr.json") as f:
        layout = json.load(f)
    head = (f"{name}: {layout['source']} hud_boundary {layout['hud_boundary']} "
            f"hfov {layout['hfov']:.1f} vfov {layout['vfov']:.1f} in a "
            f"{layout.get('window_deg', 0):.0f}-degree window ({layout.get('magnify', 1):.2f}x, "
            f"picture {layout.get('picture_scale', 1):.3f} of it) "
            f"{layout['window_size'][0]:.3f} x {layout['window_size'][1]:.3f} m at "
            f"{layout['distance']:.2f} m, fade {layout['fade']:.2f}, hud {layout['hud']}")
    ok = True
    left = load(prefix + ".gpu.L.png")
    if layout["eyes_black"]:
        right = load(prefix + ".gpu.R.png")
        worst = max(float(left[..., :3].max()), float(right[..., :3].max()))
        ok = worst == 0
        print(f"{head}\n  black: eyes_black, the eyes' brightest {worst:.0f} "
              f"{'ok' if ok else 'NOT BLACK'}")
    elif layout["window"]:
        # the head at the recenter: the stand-in's left eye, or a headset's
        # centre eye (.gpu.C.png)
        eye, size = left, layout["eye_size"]
        corners = layout.get("picture_px") or layout["window_px"]
        if "centre" in layout:
            centre = layout["centre"]
            eye = load(prefix + ".gpu.C.png")
            corners = centre.get("picture_px") or centre["window_px"]
            size = layout["centre"]["eye_size"]
        w = int(round(np.hypot(*np.subtract(corners[1], corners[0]))))
        h = int(round(np.hypot(*np.subtract(corners[3], corners[0]))))
        crop = window_crop(eye, corners, w, h)
        world = box_resize(load(prefix + ".gpu.eyes.png"), w, h)
        mean, off, worst = compare(crop, world)
        # the window a tenth of its width to the side
        dx = np.subtract(corners[1], corners[0]) / 10
        shifted = compare(window_crop(eye, [list(np.add(c, dx)) for c in corners], w, h), world)
        ok = mean <= max_mean
        print(f"{head}\n  window: {w}x{h} px in the {size[0]}x{size[1]} "
              f"{'centre' if 'centre' in layout else 'left'} eye (top left at "
              f"{corners[0][0]:.0f},{corners[0][1]:.0f}): "
              f"mean {mean:.2f}, >16 {off:.1f}%, worst cell {worst:.2f}; shifted mean "
              f"{shifted[0]:.2f} {'ok' if ok else 'OVER ' + str(max_mean)}")
        # the window as the eye sees it beside the game camera's picture
        Image.fromarray(np.concatenate([crop, world], axis=1).astype(np.uint8)).save(
            prefix + ".vr.window.png")
    else:
        a = layout.get("anchor", [0, 0, 0, 0])
        print(f"{head}\n  anchor: the eyes from {layout.get('viewpoint', 'vr_anchor')}'s place "
              f"{a[0]:.0f},{a[1]:.0f},{a[2]:.0f} facing {a[3]:.0f} degrees, no window")
        venue = layout.get("venue")
        if venue and venue.get("members"):
            print(f"  venue: {len(venue['members'])} band members, stage floor "
                  f"{venue['centre'][2]:.0f}, toward the audience {venue['toward']} "
                  f"({'crowd' if venue['toward_crowd'] else 'default'}), {venue['crowd']} crowd "
                  f"points, {venue['crowd_ahead']} in front from {venue['crowd_front']:.0f} to "
                  f"{venue['crowd_back']:.0f}, their floor {venue['audience_floor']:.0f}")
    cost = layout.get("eye_cost")
    if cost:
        print(f"  left eye: RePose {cost['repose_ms']:.2f} ms, CPU {cost['ms']:.2f} ms, GPU "
              f"{cost['gpu_ms']:.2f} ms, {cost['draws']} draws, {cost['crowd_cleared']} crowd "
              f"draws cleared near the eye")
    ok &= track_check(prefix, layout)
    split_report(layout)
    simulate(prefix, layout, left)
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("prefixes", nargs="+", help="<screenshots dir>/<capture name>")
    ap.add_argument("--max-mean", type=float, default=6)
    args = ap.parse_args()
    ok = True
    for p in args.prefixes:
        ok &= check(p, args.max_mean)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
