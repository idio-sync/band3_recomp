"""resolution: is the native picture at least as good as the emulated one at larger output sizes?

out/research/n5_design.md section 1 C. For each capture (<name>.cap with the game's own
1280x720 screenshot <name>.png beside it) and each output size WxH it makes three pictures:

  emu   the emulated picture as seen: the game's 720p PNG stretched to WxH bilinearly, as
        the presenter does (present_effect's default, "bilinear": half-pixel centres,
        edges clamped)
  nat   the native picture as seen: native_view_replay --scale H/720, the CPU's reference
        drawing at that size (screen-space passes scaled, the overlay 2x multisampled)
  ref   the "truth": native_view_replay --scale <ref-scale> (default 3, 3840x2160) box-
        (area-) downsampled to WxH, a supersampled drawing of the same scene

and a control, n720 (the native picture drawn at 1280x720 and stretched like emu), which
separates what native gains from resolution from what it gains by sharing the reference's
shading. Against ref, each of emu, nat and n720 gets:

  mean    mean |diff| over the whole frame (RGB, 0..255)
  edge    mean |diff| over the edge band: ref's luma Sobel > 40, dilated 1 px
          (out/research/n5_aa/aa_metric.py's definitions, imported)
  ramp    the ramp ratio of the picture itself (aa_metric.py: share of steep steps whose
          centre pixel is a partial value), shown next to ref's
  detail  the variance of the luma Laplacian in three crops: "scene" (crowd/venue/world
          texture, per capture), "score" (the score box, 1010,150,230,45 at 1280x720) and
          "track" (480,360,320,200), each scaled to WxH; ref's is shown too. A crop
          whose ref value is under 5 is flat and isn't judged; "closer" counts the crops
          where nat's value is nearer ref's than emu's is (|log ratio|)

and emu against nat directly (mean, edge on ref's band). The bars (the plan's): nat's edge
<= emu's, and nat's detail above emu's in >= 2 of the 3 crops.

Biases: ref is drawn by the same renderer as nat (so nat's shading matches it exactly and
emu's, the game's own, doesn't: compare nat with n720 to see the part that is resolution);
at a size whose scale is --ref-scale's (3840x2160 at 3) nat *is* ref, so use a larger
--ref-scale (6) for that size. Laplacian variance also rises with aliasing and noise, so
"more detail" is read against ref's value.

  python tools/resolution.py                          the default six captures, three sizes
  python tools/resolution.py --sizes 3840x2160 --ref-scale 6 --tag ref6
                                                      4K against a 6x reference
  python tools/resolution.py --sizes 1600x900,1920x1080 --replay-args=--no-grain --tag nograin
                                                      native without its film grain
  python tools/resolution.py --sheet render-song-40s:700,60,540,280
                                                      the contact sheets' capture and crop

Renders are kept in <out>/render/<name>@<scale>.png and reused (--force redraws them).
Writes <out>/results[-tag].json, <out>/table[-tag].txt and <out>/sheet-<W>x<H>[-tag].png
(2x2: emu | nat / ref | |nat - emu| x4, the crop at its output size).

Standard library, numpy and PIL.
"""

import argparse
import json
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPLAY = os.path.join(ROOT, "out", "native_view_replay.exe")
DEFAULT_OUT = os.path.join(ROOT, "out", "n5", "resolution")
sys.path.insert(0, os.path.join(ROOT, "out", "research", "n5_aa"))
import aa_metric  # noqa: E402  (luma, sobel, dilate, steps, ramp)

GAME = (1280, 720)
SIZES = "1600x900,1920x1080,3840x2160"
SOBEL = 40.0
STEP = 48.0

# capture, scene crop at 1280x720 (x, y, w, h): world texture away from the HUD
CAPTURES = [
    ("out/n5/velocity/caps/render-song-25s", (40, 40, 400, 300)),    # character, guitar
    ("out/n5/velocity/caps/render-song-40s", (60, 300, 380, 300)),   # venue backdrop
    ("out/n5/velocity/caps/screen-main-hub", (880, 240, 340, 300)),  # neon tiger mural
    ("out/n5/velocity/caps/screen-song-loading", (560, 200, 500, 350)),  # band, cases
    ("out/n5/aa/screens/screen-title", (100, 450, 500, 250)),        # city buildings
    # no world on the results screen: the song title, score and stars
    ("out/n5/aa/screens/screen-results", (360, 20, 560, 180)),
]
HUD_CROPS = {"score": (1010, 150, 230, 45), "track": (480, 360, 320, 200)}
CROP_NAMES = ("scene", "score", "track")
FLAT = 5.0  # a crop whose reference Laplacian variance is below this has no detail to judge


def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB")).astype(np.float32)


def to_img(rgb):
    return Image.fromarray(np.clip(np.rint(rgb), 0, 255).astype(np.uint8))


def bilinear(rgb, size):
    """The presenter's stretch: 8-bit out, as the swap chain holds it."""
    return np.asarray(to_img(rgb).resize(size, Image.BILINEAR)).astype(np.float32)


def box(rgb, size):
    """Area average to size (non-integer factors weight the partly covered pixels)."""
    if (rgb.shape[1], rgb.shape[0]) == size:
        return rgb
    chans = [np.asarray(Image.fromarray(np.ascontiguousarray(rgb[..., c]), "F")
                        .resize(size, Image.BOX)) for c in range(3)]
    return np.stack(chans, axis=2).astype(np.float32)


def scale_rect(rect, size):
    x, y, w, h = rect
    sx, sy = size[0] / GAME[0], size[1] / GAME[1]
    return round(x * sx), round(y * sy), round(w * sx), round(h * sy)


def lap_var(l, rect):
    x, y, w, h = rect
    c = l[y:y + h, x:x + w]
    lap = c[:-2, 1:-1] + c[2:, 1:-1] + c[1:-1, :-2] + c[1:-1, 2:] - 4 * c[1:-1, 1:-1]
    return float(lap.var())


def render(replay, cap, scale, out_dir, force, extra=(), sub="render"):
    path = os.path.join(out_dir, sub, f"{os.path.basename(cap)[:-4]}@{scale:g}.png")
    if force or not os.path.exists(path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        r = subprocess.run([replay, cap, path, "--scale", f"{scale:g}"] + list(extra),
                           capture_output=True, text=True)
        if r.returncode != 0 or not os.path.exists(path):
            raise RuntimeError(f"{cap} --scale {scale:g}: {(r.stderr or r.stdout).strip()[-300:]}")
    return path


def against(img, ref, band):
    d = np.abs(img - ref).mean(axis=2)
    return float(d.mean()), float(d[band].mean())


def measure(name, game_png, scene, size, nat, ref_full, n720):
    W, H = size
    emu = bilinear(load_rgb(game_png), size)
    ref = box(ref_full, size)
    n720u = bilinear(n720, size)
    lref = aa_metric.luma(ref)
    band = aa_metric.dilate(aa_metric.sobel(lref) > SOBEL)
    crops = {"scene": scale_rect(scene, size)}
    crops.update({k: scale_rect(v, size) for k, v in HUD_CROPS.items()})
    row = {"capture": name, "size": f"{W}x{H}", "band_px": int(band.sum()),
           "nat_is_ref": bool(np.array_equal(nat, ref))}
    for key, img in (("emu", emu), ("nat", nat), ("n720", n720u), ("ref", ref)):
        l = aa_metric.luma(img)
        r = {"ramp": aa_metric.ramp(aa_metric.steps(l, STEP))}
        if key != "ref":
            r["mean"], r["edge"] = against(img, ref, band)
        r["detail"] = {c: lap_var(l, crops[c]) for c in CROP_NAMES}
        row[key] = r
    row["emu_nat"] = dict(zip(("mean", "edge"), against(emu, nat, band)))
    row["bar_edge"] = row["nat"]["edge"] <= row["emu"]["edge"]
    # flat crops (nothing in the reference) don't count: the bar is 2 of 3, 2 of 2 or 1 of 1
    judged = [c for c in CROP_NAMES if row["ref"]["detail"][c] >= FLAT]
    row["detail_judged"] = len(judged)
    row["detail_wins"] = int(sum(row["nat"]["detail"][c] > row["emu"]["detail"][c] for c in judged))
    row["bar_detail"] = bool(judged) and row["detail_wins"] >= -(-2 * len(judged) // 3)
    # closer to the reference's detail (|log ratio|): more isn't better past the truth
    gap = lambda k, c: abs(np.log(max(row[k]["detail"][c], 1e-6) / row["ref"]["detail"][c]))
    row["detail_closer"] = int(sum(gap("nat", c) < gap("emu", c) for c in judged))
    return row, emu, ref


def sheet(path, size, rect, emu, nat, ref, label):
    x, y, w, h = scale_rect(rect, size)
    panels = [emu, nat, ref, np.clip(np.abs(nat - emu) * 4, 0, 255)]
    names = ["emulated: game 720p, bilinear", "native at size", "reference (box-down)",
             "|native - emulated| x4"]
    pad = 18
    out = Image.new("RGB", (2 * w + 3, 2 * (h + pad) + 3), (40, 40, 40))
    draw = ImageDraw.Draw(out)
    for i, (p, n) in enumerate(zip(panels, names)):
        px, py = (i % 2) * (w + 3), (i // 2) * (h + pad + 3)
        out.paste(to_img(p[y:y + h, x:x + w]), (px, py + pad))
        draw.text((px + 4, py + 3), f"{n}  [{label}]", fill=(255, 255, 255))
    out.save(path)


def table(rows):
    lines = []
    head = (f"{'capture':20} {'size':9} | {'mean e/n/c':>17} | {'edge e/n/c':>17} | "
            f"{'e-n mean/edge':>13} | {'ramp e/n/c/r':>23} | "
            f"{'detail scene e/n/c/r':>27} | {'detail score e/n/c/r':>27} | "
            f"{'detail track e/n/c/r':>27} | bars")
    lines.append(head)
    lines.append("-" * len(head))
    for r in rows:
        e, n, c, f = r["emu"], r["nat"], r["n720"], r["ref"]
        det = " | ".join(f"{e['detail'][k]:6.0f} {n['detail'][k]:6.0f} {c['detail'][k]:6.0f} "
                         f"{f['detail'][k]:6.0f}" for k in CROP_NAMES)
        bars = (f"edge {'ok' if r['bar_edge'] else 'MISS'}, detail {r['detail_wins']}/"
                f"{r['detail_judged']} {'ok' if r['bar_detail'] else 'MISS'}, closer "
                f"{r['detail_closer']}/{r['detail_judged']}"
                + (" (nat = ref)" if r["nat_is_ref"] else ""))
        lines.append(
            f"{r['capture']:20} {r['size']:9} | {e['mean']:5.2f} {n['mean']:5.2f} {c['mean']:5.2f} | "
            f"{e['edge']:5.2f} {n['edge']:5.2f} {c['edge']:5.2f} | "
            f"{r['emu_nat']['mean']:6.2f} {r['emu_nat']['edge']:6.2f} | "
            f"{e['ramp']:5.3f} {n['ramp']:5.3f} {c['ramp']:5.3f} {f['ramp']:5.3f} | {det} | {bars}")
    lines.append("")
    lines.append("e = emulated (game 720p, bilinear), n = native at size, c = control (native 720p, "
                 "bilinear), r = reference")
    for size in dict.fromkeys(r["size"] for r in rows):
        rs = [r for r in rows if r["size"] == size]
        m = lambda k, f: sum(r[k][f] for r in rs) / len(rs)
        lines.append(
            f"{size}: mean of {len(rs)}: mean e {m('emu', 'mean'):.2f} n {m('nat', 'mean'):.2f} "
            f"c {m('n720', 'mean'):.2f}; edge e {m('emu', 'edge'):.2f} n {m('nat', 'edge'):.2f} "
            f"c {m('n720', 'edge'):.2f}; edge bar {sum(r['bar_edge'] for r in rs)}/{len(rs)}, "
            f"detail bar {sum(r['bar_detail'] for r in rs)}/{len(rs)} "
            f"(crop wins {sum(r['detail_wins'] for r in rs)}/{sum(r['detail_judged'] for r in rs)}, "
            f"nat closer to ref's detail {sum(r['detail_closer'] for r in rs)}/"
            f"{sum(r['detail_judged'] for r in rs)})")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--replay", default=DEFAULT_REPLAY, help="native_view_replay executable")
    ap.add_argument("--out", default=DEFAULT_OUT, help="directory for renders, results, sheets")
    ap.add_argument("--sizes", default=SIZES, help="output sizes, WxH,... (16:9)")
    ap.add_argument("--ref-scale", type=float, default=3.0, help="the reference's --scale")
    ap.add_argument("--tag", default="", help="suffix for this run's results/table/sheets")
    ap.add_argument("--sheet", default="render-song-40s:700,60,540,280",
                    help="capture:x,y,w,h (at 1280x720) for the contact sheets")
    ap.add_argument("--jobs", type=int, default=4, help="replay runs at once")
    ap.add_argument("--force", action="store_true", help="redraw renders that exist")
    ap.add_argument("--replay-args", default="",
                    help="more options for every replay run, space-separated (--no-grain, say); "
                         "its renders go in render-<tag>/, so give a --tag")
    ap.add_argument("captures", nargs="*", help="only these capture names (default: all six)")
    a = ap.parse_args()

    if not os.path.exists(a.replay):
        sys.exit(f"no replay at {a.replay} (build it: tools/native_view_replay/replay.cpp)")
    caps = [(os.path.join(ROOT, p), scene) for p, scene in CAPTURES
            if not a.captures or os.path.basename(p) in a.captures]
    sizes = [tuple(int(v) for v in s.split("x")) for s in a.sizes.split(",")]
    scales = {round(h / GAME[1], 6) for _, h in sizes} | {1.0, a.ref_scale}

    work = [(p + ".cap", s) for p, _ in caps for s in sorted(scales)]
    extra = a.replay_args.split()
    if extra and not a.tag:
        sys.exit("--replay-args needs a --tag (its renders are kept apart)")
    sub = f"render-{a.tag}" if extra else "render"
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        paths = dict(zip(work, pool.map(
            lambda w: render(a.replay, w[0], w[1], a.out, a.force, extra, sub), work)))

    sheet_name, sheet_rect = a.sheet.split(":")
    sheet_rect = tuple(int(v) for v in sheet_rect.split(","))
    tag = f"-{a.tag}" if a.tag else ""
    rows = []
    for p, scene in caps:
        name = os.path.basename(p)
        ref_full = load_rgb(paths[(p + ".cap", a.ref_scale)])
        n720 = load_rgb(paths[(p + ".cap", 1.0)])
        for size in sizes:
            nat = load_rgb(paths[(p + ".cap", round(size[1] / GAME[1], 6))])
            if (nat.shape[1], nat.shape[0]) != size:
                sys.exit(f"{name}: replay drew {nat.shape[1]}x{nat.shape[0]}, not {size}")
            row, emu, ref = measure(name, p + ".png", scene, size, nat, ref_full, n720)
            rows.append(row)
            if name == sheet_name:
                sheet(os.path.join(a.out, f"sheet-{size[0]}x{size[1]}{tag}.png"), size,
                      sheet_rect, emu, nat, ref, f"{name} {size[0]}x{size[1]}")
            print(f"{name} {size[0]}x{size[1]} done", file=sys.stderr)
    # the 720p pictures' own ramp ratios, for the plan's "within 0.1 of the game's"
    at720 = {os.path.basename(p): {
        "game": aa_metric.ramp(aa_metric.steps(aa_metric.luma(load_rgb(p + ".png")), STEP)),
        "native": aa_metric.ramp(aa_metric.steps(aa_metric.luma(
            load_rgb(paths[(p + ".cap", 1.0)])), STEP))} for p, _ in caps}

    text = table(rows) + "\n\nramp ratio at 1280x720 (game PNG / native --scale 1):\n" + "\n".join(
        f"  {k:20} {v['game']:.3f} {v['native']:.3f}" for k, v in at720.items())
    print(text)
    os.makedirs(a.out, exist_ok=True)
    with open(os.path.join(a.out, f"table{tag}.txt"), "w") as f:
        f.write(text + "\n")
    with open(os.path.join(a.out, f"results{tag}.json"), "w") as f:
        json.dump({"replay": a.replay, "ref_scale": a.ref_scale, "rows": rows, "ramp_720": at720},
                  f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
