"""pairs: the native renderer's picture against the emulated GPU's, taken a moment apart.

F8 pairs: on a still moment (a menu, a paused song) the harness's `screenshot
native` is the native renderer's next frame; then `bind renderer` (F8) hands
the window to the emulated GPU, which draws whole frames again, and
`screenshot emulated` is the game's own picture once it has (the drawer hands
back once its picture is the game's: 50-450 ms); then F8 back and `screenshot native`
again. Unlike `capture`, nothing asks the emulated GPU for whole frames
beforehand, so a pair shows what the window shows under each renderer as the
player switches, skip_draws and all. With the game launched --renderer=both
on port <port> and its window off every monitor at the game's 1280x720
(`window offscreen`, `window size 1280x720`), so both pictures are the same
size:

  python tools/pairs.py out/n5/pairs --take title --port <port>
                                         takes <name>-native.png, -emulated.png and
                                         -native2.png into the set, and measures them
  python tools/pairs.py out/n5/pairs     every <name>-native.png with its <name>-emulated.png
  python tools/pairs.py out/n5/pairs title hub       only these
  python tools/pairs.py --pair a.png b.png           one pair, anywhere

Each pair gets the line replay's --compare prints for a capture (tools/parity.py's
columns: mean, % of pixels off by more than 8/32/64 in their largest channel,
p50/p90/p99, the worst cell of a 4x4 grid, signed = native minus emulated), its
Tier as parity.py grades it (without the HUD crops, which a menu hasn't), and the
pair bar: a mean of 3 or less and no 4x4 cell over 20. `blk` is the worst
32x32-pixel block's mean, where an element in one picture and not the other
shows (a sprite, a flare) though the whole frame's numbers hardly move. With
<name>-native2.png the emulated picture is measured against both native ones
(`before`, `after`) and the pair graded by the closer, since a paused song's
lights still change and either can be the emulated one's moment; `drift` is
native against native2, what moved on its own.

The emulated picture is the game's 1280x720; a native picture of another size
(the window's) is box-filtered to it first, and says so. Animated regions (a
venue behind a menu, a character idling) can be left out of every number: a
line `<name> x,y,w,h [x,y,w,h ...]  # why` per pair in <set>/exclude.txt, in
the emulated picture's pixels; or --still <mean> leaves out the 32x32 blocks
where native and native2 differ by more than that. A 4x4 cell mostly left out
isn't graded. The contact sheet (<set>/pairs_sheet.png, or --sheet) has a row
per pair: native, emulated, and their difference times 4 with the blocks over
16 boxed and the left-out regions dimmed. --crops adds parity.py's two HUD
crops (the score box and the track, scaled to the picture) to each row and to
its Tier, as parity.py grades them (each 5 or less for A).

Cross pairs: two runs of the render scripts, the reference R on the emulated
GPU and N without it (--renderer=native; docs/native-renderer.md, Render
checks), each with its captures in a directory:

  python tools/pairs.py --cross out/n7/ab/song/N out/n7/ab/song/R
  python tools/pairs.py --cross N R screen-song screen-pause --crops --json cross.json

pairs N/<name>.gpu.png (the native renderer's drawing of N's capture) with
R/<name>.png (the emulated GPU's picture of R's), for every name with both.
They're two moments, the same only as far as the game repeats itself, so
what moves by itself is left out: the 32x32 blocks where R/<name>.png and
R/<name>-again.png (the render scripts' shot a command after each capture)
differ by a mean over --still (2 by default here), and their neighbours, with
R/exclude.txt's rectangles. A screen where that leaves anything out is moving.
`rel` is the same measure of R/<name>.gpu.png against R/<name>.png: the native
renderer against the reference within one run, the mean N can be expected to
reach, and `delta` N's mean less it. A still screen passes at Tier A; a moving
one at Tier A, or at Tier B with a delta of 1 or less. The sheet
(N/cross_sheet.png, or --sheet) has N.gpu | R.png | their difference. It exits
1 if any name doesn't pass.

Needs numpy and Pillow.
"""

import argparse
import glob
import json
import os
import shutil
import sys
import time

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from parity import CROPS, CROP_BASE  # noqa: E402  (the HUD crops, at 1280x720)

TIER_A = {"mean": 8, "p50": 4, "over32": 5, "worst_cell": 20, "signed": 3, "hud": 5}
TIER_B = {"mean": 12, "over32": 12}
PAIR_MEAN, PAIR_CELL = 3.0, 20.0
BLOCK, BLOCK_BOX = 32, 16
TILE_W = 480
# --cross: the motion mask's default, and how much worse than the reference's
# own renderer a moving screen may be at Tier B
CROSS_STILL = 2.0
CROSS_DELTA = 1.0


def load(path):
    return Image.open(path).convert("RGB")


def to_size(img, size):
    """img at size: as is, or box-filtered (each output pixel the mean of the
    input pixels it covers, as a downsample should)."""
    if img.size == size:
        return img
    return img.resize(size, Image.BOX)


def parse_rect(text):
    x, y, w, h = (int(v) for v in text.split(","))
    return x, y, w, h


def read_excludes(path):
    """name -> [(x, y, w, h), ...] from an exclude.txt."""
    out = {}
    if not path or not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].split()
            if len(line) >= 2:
                out.setdefault(line[0], []).extend(parse_rect(r) for r in line[1:])
    return out


def measure(native, emulated, keep):
    """replay's DiffStats (tools/native_view_replay/replay.cpp, Measure) over
    the kept pixels, and the worst 32x32 block."""
    a = np.asarray(native, dtype=np.int16)
    b = np.asarray(emulated, dtype=np.int16)
    d = a - b
    absd = np.abs(d)
    most = absd.max(axis=2)
    per_px = absd.sum(axis=2).astype(np.float64)
    n = int(keep.sum())
    if not n:
        return None
    h, w = keep.shape
    s = {"pixels": n}
    s["mean"] = per_px[keep].sum() / (n * 3)
    s["signed"] = d[keep].sum() / (n * 3)
    m = most[keep]
    for t in (8, 32, 64):
        s[f"over{t}"] = 100.0 * (m > t).sum() / n
    hist = np.bincount(m.ravel(), minlength=256)
    cum = np.cumsum(hist)
    for p, key in ((0.5, "p50"), (0.9, "p90"), (0.99, "p99")):
        s[key] = int(np.searchsorted(cum, p * n))
    # the 4x4 grid's cells, as replay splits the picture
    ys = np.minimum(3, np.arange(h) * 4 // h)
    xs = np.minimum(3, np.arange(w) * 4 // w)
    worst, at = 0.0, (0, 0)
    for cy in range(4):
        for cx in range(4):
            sel = keep[ys == cy][:, xs == cx]
            k = int(sel.sum())
            # a cell mostly left out says little: an eighth of it at least
            if k * 8 < sel.size:
                continue
            v = per_px[ys == cy][:, xs == cx][sel].sum() / (k * 3)
            if v > worst:
                worst, at = v, (cx, cy)
    s["worst_cell"], s["worst_at"] = worst, at
    blocks = block_means(per_px / 3, keep)
    by, bx = np.unravel_index(np.nanargmax(np.nan_to_num(blocks, nan=-1)), blocks.shape)
    s["block"], s["block_at"] = float(np.nan_to_num(blocks[by, bx])), (int(bx) * BLOCK, int(by) * BLOCK)
    s["blocks"] = blocks
    return s


def block_means(diff, keep):
    """The mean of diff over each 32x32 block's kept pixels (NaN where none)."""
    h, w = diff.shape
    bh, bw = h // BLOCK, w // BLOCK
    dd = np.where(keep, diff, 0)[:bh * BLOCK, :bw * BLOCK].reshape(bh, BLOCK, bw, BLOCK)
    kk = keep[:bh * BLOCK, :bw * BLOCK].reshape(bh, BLOCK, bw, BLOCK)
    count = kk.sum(axis=(1, 3))
    with np.errstate(invalid="ignore", divide="ignore"):
        return np.where(count > BLOCK * BLOCK // 4, dd.sum(axis=(1, 3)) / count, np.nan)


def still_mask(native, again, limit):
    """False on the 32x32 blocks where native and again (the native picture
    a moment later) differ by a mean over limit, and on their neighbours:
    what moved by itself, which a pair can't compare."""
    d = np.abs(np.asarray(native, np.int16) - np.asarray(again, np.int16)).mean(axis=2)
    h, w = d.shape
    moved = np.nan_to_num(block_means(d, np.ones((h, w), bool)), nan=0) > limit
    grown = moved.copy()
    grown[1:, :] |= moved[:-1, :]
    grown[:-1, :] |= moved[1:, :]
    grown[:, 1:] |= moved[:, :-1]
    grown[:, :-1] |= moved[:, 1:]
    keep = ~np.repeat(np.repeat(grown, BLOCK, axis=0), BLOCK, axis=1)
    out = np.ones((h, w), bool)
    out[:keep.shape[0], :keep.shape[1]] = keep
    return out


def crop_means(native, emulated, keep):
    """parity.py's HUD crops' means (crop -> mean), scaled to the emulated
    picture's size, over their kept pixels; None for a crop all left out."""
    w, h = emulated.size
    per_px = np.abs(np.asarray(native, np.int16) - np.asarray(emulated, np.int16)).sum(axis=2)
    out = {}
    for crop, (x, y, cw, ch) in CROPS.items():
        sx, sy = w / CROP_BASE[0], h / CROP_BASE[1]
        x0, y0 = round(x * sx), round(y * sy)
        x1, y1 = x0 + round(cw * sx), y0 + round(ch * sy)
        k = keep[y0:y1, x0:x1]
        n = int(k.sum())
        out[crop] = float(per_px[y0:y1, x0:x1][k].sum() / (n * 3)) if n else None
    return out


def grade(s, crops=None):
    """The tier, and what keeps it from A; with crops, their means count as
    parity.py's do (a crop all left out isn't graded)."""
    misses = []
    if s["mean"] > TIER_A["mean"]: misses.append("mean")
    if s["p50"] > TIER_A["p50"]: misses.append("p50")
    if s["over32"] > TIER_A["over32"]: misses.append(">32")
    if s["worst_cell"] > TIER_A["worst_cell"]: misses.append("cell")
    for crop, v in (crops or {}).items():
        if v is not None and v > TIER_A["hud"]: misses.append(crop)
    if abs(s["signed"]) > TIER_A["signed"]: misses.append("signed")
    if not misses:
        return "A", misses
    if s["mean"] <= TIER_B["mean"] and s["over32"] <= TIER_B["over32"]:
        return "B", misses
    return "-", misses


def line(name, kind, s):
    return (f"{name:22} {kind:6} {s['mean']:7.3f} {s['over8']:6.2f} {s['over32']:6.2f} "
            f"{s['over64']:6.2f} {s['p50']:4d} {s['p90']:4d} {s['p99']:4d} "
            f"{s['worst_cell']:6.2f} {s['signed']:+7.2f} {s['block']:6.1f}")


def crops_text(crops):
    if crops is None:
        return ""
    return "".join(f" {v:6.2f}" if v is not None else f" {'-':>6}" for v in crops.values())


def sheet_row(name, native, emulated, keep, s, note, labels="native | emulated"):
    """native | emulated | |difference| x4, the blocks over 16 boxed, with a label."""
    w, h = emulated.size
    th = round(TILE_W * h / w)
    a = np.asarray(native, dtype=np.int16)
    b = np.asarray(emulated, dtype=np.int16)
    diff = np.clip(np.abs(a - b).mean(axis=2) * 4, 0, 255).astype(np.uint8)
    dimg = Image.fromarray(diff).convert("RGB")
    if not keep.all():
        # the left-out regions dimmed blue in all three
        tint = np.zeros((h, w, 3), np.uint8)
        tint[~keep] = (0, 0, 90)
        mask = Image.fromarray(np.where(keep, 0, 140).astype(np.uint8))
        native = Image.composite(Image.fromarray(tint), native, mask)
        emulated = Image.composite(Image.fromarray(tint), emulated, mask)
        dimg = Image.composite(Image.fromarray(tint), dimg, mask)
    dr = ImageDraw.Draw(dimg)
    blocks = s["blocks"]
    for by, bx in zip(*np.nonzero(np.nan_to_num(blocks) > BLOCK_BOX)):
        dr.rectangle([bx * BLOCK, by * BLOCK, bx * BLOCK + BLOCK - 1, by * BLOCK + BLOCK - 1],
                     outline=(255, 0, 0), width=2)
    label = 18
    row = Image.new("RGB", (TILE_W * 3, th + label), (24, 24, 24))
    for k, t in enumerate((native, emulated, dimg)):
        row.paste(t.resize((TILE_W, th), Image.BOX), (k * TILE_W, label))
    text = (f"{name}  {labels} | diff x4   mean {s['mean']:.2f}  cell {s['worst_cell']:.1f}"
            f"  blk {s['block']:.0f} at {s['block_at']}  signed {s['signed']:+.2f}  {note}")
    ImageDraw.Draw(row).text((4, 3), text, fill=(255, 255, 255))
    return row


def find_pairs(directory, names):
    pairs = []
    for native in sorted(glob.glob(os.path.join(directory, "*-native.png"))):
        name = os.path.basename(native)[:-len("-native.png")]
        if names and name not in names:
            continue
        emulated = os.path.join(directory, name + "-emulated.png")
        if not os.path.exists(emulated):
            print(f"{name}: no {os.path.basename(emulated)}", file=sys.stderr)
            continue
        again = os.path.join(directory, name + "-native2.png")
        pairs.append((name, native, emulated, again if os.path.exists(again) else None))
    return pairs


def take(port, directory, name):
    """One F8 pair from the game on port, into directory: <name>-native.png,
    -emulated.png and -native2.png. The emulated shot is retried until the
    emulated GPU's picture is the game's again (it draws whole frames once F8
    hands it the window; a paused music-video venue took 450 ms) and taken
    again 250 ms later, and F8 goes back to native whatever happens."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import band3ctl

    conn = band3ctl.connect(port)
    shots = {}

    def shot(source, kind):
        r = conn.command(f"screenshot {source} {name}-{kind}")
        if r.get("ok"):
            shots[kind] = r["path"]
        return r

    try:
        r = shot("native", "native")
        if not r.get("ok") or r.get("renderer") != "native":
            sys.exit(f"{name}: no native picture ({r.get('error') or r.get('renderer')}): "
                     "is renderer both, the native picture shown?")
        conn.command("bind renderer")
        started = time.monotonic()
        try:
            while True:
                time.sleep(0.1)
                r = shot("emulated", "emulated")
                if r.get("ok"):
                    # and again a moment later, which the pair keeps
                    time.sleep(0.25)
                    r = shot("emulated", "emulated")
                    break
                if time.monotonic() - started > 5:
                    sys.exit(f"{name}: the emulated picture wasn't the game's after 5 s: "
                             f"{r.get('error')}")
            waited = time.monotonic() - started
        finally:
            conn.command("bind renderer")
        time.sleep(0.3)
        shot("native", "native2")
    finally:
        conn.close()
    os.makedirs(directory, exist_ok=True)
    for kind, path in shots.items():
        shutil.move(path, os.path.join(directory, f"{name}-{kind}.png"))
    print(f"{name}: emulated picture {waited * 1000:.0f} ms after F8")


def find_cross(n_dir, r_dir, names):
    """(name, N .gpu.png, R .png, R -again.png or None, R .gpu.png or None)
    for every N/<name>.gpu.png with an R/<name>.png."""
    found = []
    for gpu in sorted(glob.glob(os.path.join(n_dir, "*.gpu.png"))):
        name = os.path.basename(gpu)[:-len(".gpu.png")]
        if names and name not in names:
            continue
        shot = os.path.join(r_dir, name + ".png")
        if not os.path.exists(shot):
            print(f"{name}: no {shot}", file=sys.stderr)
            continue
        again = os.path.join(r_dir, name + "-again.png")
        r_gpu = os.path.join(r_dir, name + ".gpu.png")
        found.append((name, gpu, shot, again if os.path.exists(again) else None,
                      r_gpu if os.path.exists(r_gpu) else None))
    return found


def cross(a, n_dir, r_dir, names):
    """--cross: N's native drawing against R's emulated picture, name by name."""
    found = find_cross(n_dir, r_dir, set(names))
    if not found:
        sys.exit(f"no N/<name>.gpu.png in {n_dir} with an R/<name>.png in {r_dir}")
    excludes = read_excludes(a.exclude or os.path.join(r_dir, "exclude.txt"))
    limit = a.still if a.still is not None else CROSS_STILL
    head = (f"{'capture':24} {'run':6} {'mean':>7} {'>8%':>6} {'>32%':>6} {'>64%':>6} "
            f"{'p50':>4} {'p90':>4} {'p99':>4} {'cell':>6} {'signed':>7} {'blk':>6}"
            + (f" {'score':>6} {'track':>6}" if a.crops else "")
            + f" {'tier':>4} {'moved':>6} {'delta':>6}  verdict")
    print(head)
    print("-" * len(head))
    rows, results, failed = [], {}, []
    for name, gpu_path, shot_path, again_path, r_gpu_path in found:
        ref = load(shot_path)
        native = to_size(load(gpu_path), ref.size)
        w, h = ref.size
        keep = np.ones((h, w), bool)
        for x, y, rw, rh in excludes.get(name, []):
            keep[max(0, y):y + rh, max(0, x):x + rw] = False
        moved = 0.0
        if again_path:
            motion = still_mask(ref, to_size(load(again_path), ref.size), limit)
            moved = float(100 * (1 - motion.mean()))
            keep &= motion
        s = measure(native, ref, keep)
        if s is None:
            print(f"{name:24} every pixel left out")
            failed.append(name)
            continue
        crops = crop_means(native, ref, keep) if a.crops else None
        tier, misses = grade(s, crops)
        rel = delta = None
        if r_gpu_path:
            rel = measure(to_size(load(r_gpu_path), ref.size), ref, keep)
            delta = float(s["mean"] - rel["mean"])
        moving = moved > 0
        if moving:
            ok = tier == "A" or (tier == "B" and delta is not None and delta <= CROSS_DELTA)
        else:
            ok = tier == "A"
        ok = bool(ok)
        verdict = ("ok" if ok else "FAIL") + (" moving" if moving else " still") + \
            ("" if again_path else " (no -again)")
        out = (line(f"{name:24}", "N-R", s) + crops_text(crops) + f" {tier:>4} {moved:5.1f}%"
               + f" {fmt(delta, '+6.2f'):>6}  {verdict}")
        if misses:
            out += "  (A: " + " ".join(misses) + ")"
        print(out)
        if rel is not None:
            rel_crops = crop_means(to_size(load(r_gpu_path), ref.size), ref, keep) \
                if a.crops else None
            print(line(f"{name:24}", "rel", rel) + crops_text(rel_crops)
                  + f" {grade(rel, rel_crops)[0]:>4}")
        if not ok:
            failed.append(name)
        result = {k: v for k, v in s.items() if k != "blocks"}
        result.update(tier=tier, misses=misses, crops=crops, moved_pct=moved, moving=moving,
                      again=bool(again_path), delta=delta, ok=ok,
                      excluded=excludes.get(name, []))
        if rel is not None:
            result["rel"] = {k: v for k, v in rel.items() if k != "blocks"}
        results[name] = result
        note = f"{moved:.0f}% moved, left out" if moving else "still"
        rows.append(sheet_row(name, native, ref, keep, s, note, labels="N .gpu | R .png"))
    print()
    print(f"still screens pass at Tier A; moving ones (anything left out at --still {limit:g}) "
          f"at Tier A, or Tier B with delta <= {CROSS_DELTA:g} (delta: N's mean less rel's, "
          "R.gpu.png against R.png under the same mask)")
    print(f"{len(found) - len(failed)} of {len(found)} pass"
          + (f"; not: {', '.join(failed)}" if failed else ""))
    sheet_path = a.sheet or os.path.join(n_dir, "cross_sheet.png")
    write_sheet(rows, sheet_path)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 1 if failed else 0


def fmt(v, spec):
    return format(v, spec) if v is not None else "-"


def write_sheet(rows, path):
    if not rows:
        return
    sheet = Image.new("RGB", (TILE_W * 3, sum(r.size[1] for r in rows)))
    y = 0
    for r in rows:
        sheet.paste(r, (0, y))
        y += r.size[1]
    sheet.save(path)
    print(f"wrote {path}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--take", metavar="NAME",
                    help="first take the pair NAME from the running game into the set "
                         "(F8 to the emulated GPU and back), then measure it")
    ap.add_argument("--port", type=int, default=21071, help="the game's test_port, for --take")
    ap.add_argument("set", nargs="?", help="directory of <name>-native.png / <name>-emulated.png")
    ap.add_argument("names", nargs="*", help="only these pairs (default: all)")
    ap.add_argument("--pair", nargs=2, action="append", metavar=("NATIVE", "EMULATED"),
                    help="a pair of PNGs anywhere (repeatable)")
    ap.add_argument("--exclude", help="exclude.txt (default <set>/exclude.txt)")
    ap.add_argument("--sheet", help="contact sheet PNG (default <set>/pairs_sheet.png)")
    ap.add_argument("--still", type=float, metavar="MEAN",
                    help="with <name>-native2.png, also leave out every 32x32 block whose "
                         "native-native2 mean is over MEAN, and the blocks around it: what "
                         "moved on its own between the shots (with --cross, R's <name>.png "
                         "against <name>-again.png, at 2 unless given)")
    ap.add_argument("--crops", action="store_true",
                    help="parity.py's HUD crops (score box, track) in each row and its Tier")
    ap.add_argument("--cross", nargs=2, metavar=("N_DIR", "R_DIR"),
                    help="N_DIR/<name>.gpu.png against R_DIR/<name>.png for every name with "
                         "both (two runs' captures; the names after it are the only ones)")
    ap.add_argument("--json", help="write the numbers here too")
    a = ap.parse_args()

    if a.cross:
        if a.take or a.pair:
            sys.exit("--cross doesn't go with --take or --pair")
        return cross(a, a.cross[0], a.cross[1], ([a.set] if a.set else []) + a.names)

    if a.take:
        if not a.set:
            sys.exit("--take needs the set to put the pair in")
        take(a.port, a.set, a.take)
        a.names = [a.take]
    pairs = []
    if a.set:
        pairs += find_pairs(a.set, set(a.names))
    for native, emulated in a.pair or []:
        name = os.path.splitext(os.path.basename(native))[0]
        pairs.append((name, native, emulated, None))
    if not pairs:
        sys.exit("no pairs")
    excludes = read_excludes(a.exclude or (os.path.join(a.set, "exclude.txt") if a.set else None))

    head = (f"{'pair':22} {'run':6} {'mean':>7} {'>8%':>6} {'>32%':>6} {'>64%':>6} "
            f"{'p50':>4} {'p90':>4} {'p99':>4} {'cell':>6} {'signed':>7} {'blk':>6} "
            + (f"{'score':>6} {'track':>6} " if a.crops else "")
            + f"{'tier':>4} {'bar':>4}")
    print(head)
    print("-" * len(head))
    rows, results, tiers, bar_misses = [], {}, {}, []
    for name, native_path, emulated_path, again_path in pairs:
        emulated = load(emulated_path)
        native_full = load(native_path)
        native = to_size(native_full, emulated.size)
        note = "" if native_full.size == emulated.size else \
            f"native {native_full.size[0]}x{native_full.size[1]} box-filtered"
        w, h = emulated.size
        keep = np.ones((h, w), bool)
        for x, y, rw, rh in excludes.get(name, []):
            keep[max(0, y):y + rh, max(0, x):x + rw] = False
        again = to_size(load(again_path), emulated.size) if again_path else None
        if a.still and again is not None:
            keep &= still_mask(native, again, a.still)
        if not keep.all():
            note = (note + "; " if note else "") + f"{100 * (1 - keep.mean()):.0f}% left out"
        s = measure(native, emulated, keep)
        if s is None:
            print(f"{name:22} every pixel left out")
            continue
        kind = "pair"
        if again is not None:
            # the emulated picture against the native one after it as well,
            # graded by whichever is closer: a paused song's lights go on
            # changing, so either may be the same moment as the emulated one
            after = measure(again, emulated, keep)
            print(line(name, "before", s))
            print(line(name, "after", after))
            if after["mean"] < s["mean"]:
                s, native, kind = after, again, "after"
            else:
                kind = "before"
        crops = crop_means(native, emulated, keep) if a.crops else None
        tier, misses = grade(s, crops)
        ok = bool(s["mean"] <= PAIR_MEAN and s["worst_cell"] <= PAIR_CELL)
        tiers.setdefault(tier, []).append(name)
        if not ok:
            bar_misses.append(name)
        out = line(name, "pair", s) + crops_text(crops) + f" {tier:>4} {'ok' if ok else 'over':>4}"
        if misses:
            out += "  (A: " + " ".join(misses) + ")"
        if again is not None:
            note = (note + "; " if note else "") + f"native {kind} the emulated shot"
        if note:
            out += "  [" + note + "]"
        print(out)
        result = {k: v for k, v in s.items() if k != "blocks"}
        result.update(tier=tier, bar=ok, note=note, excluded=excludes.get(name, []),
                      native=kind, crops=crops)
        if again is not None:
            d = measure(again, to_size(native_full, emulated.size), keep)
            print(line(name, "drift", d))
            result["drift"] = {k: v for k, v in d.items() if k != "blocks"}
        results[name] = result
        rows.append(sheet_row(name, native, emulated, keep, s, note))
    print()
    for tier, label in (("A", "Tier A"), ("B", "Tier B"), ("-", "neither")):
        got = tiers.get(tier, [])
        print(f"{label}: {len(got)}" + (": " + ", ".join(got) if got else ""))
    print(f"pair bar (mean <= {PAIR_MEAN:g}, no 4x4 cell over {PAIR_CELL:g}): "
          f"{len(pairs) - len(bar_misses)} of {len(pairs)}"
          + (" ok; over: " + ", ".join(bar_misses) if bar_misses else " ok"))

    sheet_path = a.sheet or (os.path.join(a.set, "pairs_sheet.png") if a.set else None)
    if sheet_path:
        write_sheet(rows, sheet_path)
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
