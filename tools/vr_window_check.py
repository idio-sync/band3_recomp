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
picture is magnified to fill it (`magnify`, vr_pose.h's WindowMagnify).
Without a VR session the eyes are a stand-in's: the head at LOCAL space's
origin, both eyes at it, 60 degrees each way, so the left eye sees the window
square on from where the game camera is. With a headset the capture adds such
an eye at the recenter's head, <name>.gpu.C.png (the layout's `centre`), and
the window is checked in that.

For each name this:

  window     takes the window's quad out of .gpu.L.png (or .gpu.C.png),
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
  sim        writes <name>.vr.sim.png: the left eye as the headset composes it,
             the eyes faded by `fade`, the dark at `dim` around the window, and
             the panel's picture (premultiplied) over it where `hud_px` says

It exits 1 when a window's mean is over --max-mean (6) or a black eye isn't.

  python tools/vr_window_check.py <screenshots>/hub <screenshots>/song
  python tools/vr_window_check.py <screenshots>/hub --max-mean 4
"""

import argparse
import json
import os
import sys

import numpy as np
from PIL import Image


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


def simulate(prefix, layout, left):
    """the left eye as the compositor shows it: fade, dark, panel"""
    out = left[..., :3] * (1 - layout["fade"])
    if layout["eyes_black"]:
        out[:] = 0
    if layout["window"] and layout["frame_quads"] > 0:
        inside = window_mask(out.shape, layout["window_px"])
        out[~inside] *= 1 - layout["dim"]
    hud_path = prefix + ".gpu.hud.png"
    corners = layout.get("hud_px") or []
    if layout["hud"] != "off" and os.path.exists(hud_path) and None not in corners:
        hud = Image.open(hud_path).convert("RGBA")
        coeffs = perspective_coeffs(corners, [(0, 0), (hud.width, 0), (hud.width, hud.height),
                                              (0, hud.height)])
        h, w = out.shape[:2]
        warped = np.asarray(hud.transform((w, h), Image.PERSPECTIVE, tuple(coeffs),
                                          Image.BILINEAR), dtype=np.float32)
        # outside the quad the transform samples nothing: transparent
        warped = warped * window_mask(out.shape, corners)[..., None]
        a = warped[..., 3:4] / 255
        out = warped[..., :3] + out * (1 - a)
    Image.fromarray(np.clip(out, 0, 255).astype(np.uint8)).save(prefix + ".vr.sim.png")


def check(prefix, max_mean):
    name = os.path.basename(prefix)
    with open(prefix + ".vr.json") as f:
        layout = json.load(f)
    head = (f"{name}: {layout['source']} hud_boundary {layout['hud_boundary']} "
            f"hfov {layout['hfov']:.1f} vfov {layout['vfov']:.1f} in a "
            f"{layout.get('window_deg', 0):.0f}-degree window ({layout.get('magnify', 1):.2f}x) "
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
        eye, corners, size = left, layout["window_px"], layout["eye_size"]
        if "centre" in layout:
            eye, corners = load(prefix + ".gpu.C.png"), layout["centre"]["window_px"]
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
        print(f"{head}\n  anchor: the eyes from vr_anchor's place, no window")
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
