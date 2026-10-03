"""pairs: the native renderer's picture against the emulated GPU's, taken a moment apart.

F8 pairs: on a still moment (a menu, a paused song) the harness's `screenshot
native` is the native renderer's next frame; then `bind renderer` (F8) hands
the window to the emulated GPU, which draws whole frames again, and
`screenshot emulated` is the game's own picture once it has (the drawer hands
back after two whole frames: 50-450 ms); then F8 back and `screenshot native`
again. Unlike `capture`, nothing asks the emulated GPU for whole frames
beforehand, so a pair shows what the window shows under each renderer as the
player switches, skip_draws and all. With the game launched --renderer=native
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
16 boxed and the left-out regions dimmed.

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

TIER_A = {"mean": 8, "p50": 4, "over32": 5, "worst_cell": 20, "signed": 3}
TIER_B = {"mean": 12, "over32": 12}
PAIR_MEAN, PAIR_CELL = 3.0, 20.0
BLOCK, BLOCK_BOX = 32, 16
TILE_W = 480


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


def grade(s):
    misses = []
    if s["mean"] > TIER_A["mean"]: misses.append("mean")
    if s["p50"] > TIER_A["p50"]: misses.append("p50")
    if s["over32"] > TIER_A["over32"]: misses.append(">32")
    if s["worst_cell"] > TIER_A["worst_cell"]: misses.append("cell")
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


def sheet_row(name, native, emulated, keep, s, note):
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
    text = (f"{name}  native | emulated | diff x4   mean {s['mean']:.2f}  cell {s['worst_cell']:.1f}"
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
                     "is renderer native?")
        conn.command("bind renderer")
        started = time.monotonic()
        try:
            while True:
                time.sleep(0.1)
                r = shot("emulated", "emulated")
                if r.get("ok"):
                    # and again a moment later: with even/odd rendering the
                    # first picture after two whole frames can still be a
                    # world drawn while skipped (black), for a frame
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
                         "moved on its own between the shots")
    ap.add_argument("--json", help="write the numbers here too")
    a = ap.parse_args()

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
            f"{'tier':>4} {'bar':>4}")
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
        tier, misses = grade(s)
        ok = bool(s["mean"] <= PAIR_MEAN and s["worst_cell"] <= PAIR_CELL)
        tiers.setdefault(tier, []).append(name)
        if not ok:
            bar_misses.append(name)
        out = line(name, "pair", s) + f" {tier:>4} {'ok' if ok else 'over':>4}"
        if misses:
            out += "  (A: " + " ".join(misses) + ")"
        if again is not None:
            note = (note + "; " if note else "") + f"native {kind} the emulated shot"
        if note:
            out += "  [" + note + "]"
        print(out)
        result = {k: v for k, v in s.items() if k != "blocks"}
        result.update(tier=tier, bar=ok, note=note, excluded=excludes.get(name, []),
                      native=kind)
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
        sheet = Image.new("RGB", (TILE_W * 3, sum(r.size[1] for r in rows)))
        y = 0
        for r in rows:
            sheet.paste(r, (0, y))
            y += r.size[1]
        sheet.save(sheet_path)
        print(f"wrote {sheet_path}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
