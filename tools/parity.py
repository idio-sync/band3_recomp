"""parity: how close the native view is to the game's own frames, capture by capture.

Runs native_view_replay over a set of captures (out/parity/<name>.cap, each with
the game's screenshot <name>.png and the GPU backend's <name>.gpu.png, from the
harness's `capture`) and prints out/research/parity_plan.md's metrics for each:

  cpu      the CPU's drawing of the capture against the game  (--compare <png>)
  gpu      the GPU backend's picture against the game  (--compare <png> --image <gpu.png>)
  gpu-cpu  the GPU's picture against the CPU's  (--diff <gpu.png>)

and both HUD crops' means (the score box and the track) for cpu and gpu. Each row
is graded by the plan's tiers:

  Tier A (parity)      mean <= 8, p50 <= 4, px>32 <= 5%, worst cell <= 20,
                       both HUD crops <= 5, |signed| <= 3
  Tier B (acceptable)  mean <= 12, px>32 <= 12%

and the GPU against the CPU checked at <= 0.5. The gpu rows are the .gpu.png the
game wrote when the capture was taken: they change only when it's captured again,
not when the replay is rebuilt.

  python tools/parity.py                          the table for out/parity
  python tools/parity.py --set out/parity_other   another set
  python tools/parity.py --baseline out/parity/baseline.json
                                                  against saved numbers (written if missing):
                                                  exits 1 if any capture's cpu or gpu mean got
                                                  worse by more than 0.5
  python tools/parity.py --baseline b.json --write-baseline
                                                  saves this run's numbers over it

Numbers from different runs of the game aren't comparable (run to run the same
moment differs by 5.5-9.3): a baseline compares re-renders of the same captures.

Standard library only.
"""

import argparse
import glob
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPLAY = os.path.join(ROOT, "out", "native_view_replay.exe")
DEFAULT_SET = os.path.join(ROOT, "out", "parity")

# the HUD crops at 1280x720, x, y, w, h: scaled to the screenshot's size
CROPS = {"score": (1010, 150, 230, 45), "track": (480, 360, 320, 200)}
CROP_BASE = (1280, 720)

TIER_A = {"mean": 8, "p50": 4, "over32": 5, "worst_cell": 20, "hud": 5, "signed": 3}
TIER_B = {"mean": 12, "over32": 12}
GPU_CPU_LIMIT = 0.5
REGRESSION = 0.5

MEAN_RE = re.compile(r"mean difference ([0-9.]+) of 255")
METRICS_RE = re.compile(
    r"metrics: (\d+) pixels.*?; mean ([0-9.]+); >8 ([0-9.]+)% >32 ([0-9.]+)% >64 ([0-9.]+)%; "
    r"p50 (\d+) p90 (\d+) p99 (\d+); worst cell ([0-9.]+) \((\d),(\d) of 4x4\); signed ([-+0-9.]+)")


def png_size(path):
    with open(path, "rb") as f:
        head = f.read(24)
    if head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        raise ValueError(f"{path} isn't a PNG")
    return struct.unpack(">II", head[16:24])


def scaled_crop(crop, size):
    x, y, w, h = crop
    sx, sy = size[0] / CROP_BASE[0], size[1] / CROP_BASE[1]
    return f"{round(x * sx)},{round(y * sy)},{round(w * sx)},{round(h * sy)}"


def run_replay(replay, args):
    """One replay run: its metrics, or an error string."""
    r = subprocess.run([replay] + args, capture_output=True, text=True)
    if r.returncode != 0:
        return {"error": (r.stderr or r.stdout).strip().splitlines()[-1:] or ["failed"]}
    m = METRICS_RE.search(r.stdout)
    if not m or not MEAN_RE.search(r.stdout):
        return {"error": ["no metrics line (a replay from before them?)"]}
    g = m.groups()
    return {
        "pixels": int(g[0]), "mean": float(g[1]), "over8": float(g[2]), "over32": float(g[3]),
        "over64": float(g[4]), "p50": int(g[5]), "p90": int(g[6]), "p99": int(g[7]),
        "worst_cell": float(g[8]), "worst_at": [int(g[9]), int(g[10])], "signed": float(g[11]),
    }


def jobs_for(cap, out_dir):
    """The replay runs one capture needs, as (key, args)."""
    name = os.path.basename(cap)[:-4]
    base = cap[:-4]
    shot, gpu = base + ".png", base + ".gpu.png"
    out = lambda tag: os.path.join(out_dir, f"{name}.{tag}.png")
    if not os.path.exists(shot):
        return name, []
    size = png_size(shot)
    jobs = [(("cpu", None), [cap, out("cpu"), "--compare", shot])]
    for crop, rect in CROPS.items():
        jobs.append((("cpu", crop),
                     [cap, out("cpu-" + crop), "--compare", shot, "--crop",
                      scaled_crop(rect, size)]))
    if os.path.exists(gpu):
        jobs.append((("gpu", None), [cap, out("gpu"), "--compare", shot, "--image", gpu]))
        for crop, rect in CROPS.items():
            jobs.append((("gpu", crop),
                         [cap, out("gpu-" + crop), "--compare", shot, "--image", gpu, "--crop",
                          scaled_crop(rect, size)]))
        jobs.append((("gpu-cpu", None), [cap, out("gpu-cpu"), "--diff", gpu]))
    return name, jobs


def grade(row):
    """The tier a cpu or gpu row makes, and what keeps it from A."""
    if "error" in row:
        return "-", ["error"]
    misses = []
    if row["mean"] > TIER_A["mean"]: misses.append("mean")
    if row["p50"] > TIER_A["p50"]: misses.append("p50")
    if row["over32"] > TIER_A["over32"]: misses.append(">32")
    if row["worst_cell"] > TIER_A["worst_cell"]: misses.append("cell")
    for crop in CROPS:
        if row.get(crop) is None or row[crop] > TIER_A["hud"]: misses.append(crop)
    if abs(row["signed"]) > TIER_A["signed"]: misses.append("signed")
    if not misses:
        return "A", []
    if row["mean"] <= TIER_B["mean"] and row["over32"] <= TIER_B["over32"]:
        return "B", misses
    return "-", misses


def measure(captures, replay, jobs):
    results = {}
    with tempfile.TemporaryDirectory(prefix="parity") as out_dir:
        work = []
        for cap in captures:
            name, cap_jobs = jobs_for(cap, out_dir)
            results[name] = {}
            work += [(name, key, args) for key, args in cap_jobs]
        with ThreadPoolExecutor(max_workers=jobs) as pool:
            done = list(pool.map(lambda w: (w[0], w[1], run_replay(replay, w[2])), work))
    for name, (kind, crop), m in done:
        r = results[name]
        if crop is None:
            r.setdefault(kind, {}).update(m)
        else:
            r.setdefault(kind, {})[crop] = m.get("mean")
    return results


def fmt(v, spec):
    return format(v, spec) if v is not None else "-"


def print_table(results, baseline):
    head = (f"{'capture':16} {'run':7} {'mean':>7} {'>8%':>6} {'>32%':>6} {'>64%':>6} "
            f"{'p50':>4} {'p90':>4} {'p99':>4} {'cell':>6} {'signed':>7} {'score':>6} "
            f"{'track':>6} {'tier':>4}")
    if baseline:
        head += f" {'d mean':>7}"
    print(head)
    print("-" * len(head))
    tiers = {}
    for name in sorted(results):
        r = results[name]
        base = (baseline or {}).get(name, {})
        for kind in ("cpu", "gpu", "gpu-cpu"):
            row = r.get(kind)
            if row is None:
                continue
            if "error" in row:
                print(f"{name:16} {kind:7} error: {' '.join(row['error'])}")
                continue
            if kind == "gpu-cpu":
                tier = "ok" if row["mean"] <= GPU_CPU_LIMIT else "over"
                misses = []
            else:
                tier, misses = grade(row)
                tiers.setdefault(tier, []).append(f"{name} {kind}")
            line = (f"{name:16} {kind:7} {row['mean']:7.3f} {row['over8']:6.2f} "
                    f"{row['over32']:6.2f} {row['over64']:6.2f} {row['p50']:4d} {row['p90']:4d} "
                    f"{row['p99']:4d} {row['worst_cell']:6.2f} {row['signed']:+7.2f} "
                    f"{fmt(row.get('score'), '6.2f'):>6} {fmt(row.get('track'), '6.2f'):>6} "
                    f"{tier:>4}")
            if baseline:
                was = base.get(kind, {}).get("mean")
                line += f" {fmt(row['mean'] - was if was is not None else None, '+7.3f'):>7}"
            if misses:
                line += "  (A: " + " ".join(misses) + ")"
            print(line)
    print()
    print("gpu rows are the capture's .gpu.png as the game wrote it; they change only when "
          "the capture is taken again")
    for tier, label in (("A", "Tier A (parity)"), ("B", "Tier B (acceptable)"),
                        ("-", "neither")):
        rows = tiers.get(tier, [])
        print(f"{label}: {len(rows)}" + (": " + ", ".join(rows) if rows else ""))


def regressions(results, baseline):
    worse = []
    for name, r in results.items():
        for kind in ("cpu", "gpu"):
            now = r.get(kind, {}).get("mean")
            was = baseline.get(name, {}).get(kind, {}).get("mean")
            if now is not None and was is not None and now - was > REGRESSION:
                worse.append(f"{name} {kind} mean {was:.3f} -> {now:.3f}")
    return worse


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--set", default=DEFAULT_SET, help="directory of <name>.cap/.png/.gpu.png")
    ap.add_argument("--replay", default=DEFAULT_REPLAY, help="native_view_replay executable")
    ap.add_argument("--baseline", help="JSON of earlier numbers: compared against, written if "
                                       "missing")
    ap.add_argument("--write-baseline", action="store_true",
                    help="write this run's numbers to --baseline even if it exists")
    ap.add_argument("--jobs", type=int, default=4, help="replay runs at once")
    ap.add_argument("captures", nargs="*", help="only these capture names (default: all)")
    a = ap.parse_args()

    if not os.path.exists(a.replay):
        sys.exit(f"no replay at {a.replay} (build it: tools/native_view_replay/replay.cpp)")
    captures = sorted(glob.glob(os.path.join(a.set, "*.cap")))
    if a.captures:
        captures = [c for c in captures if os.path.basename(c)[:-4] in a.captures]
    if not captures:
        sys.exit(f"no captures in {a.set}")

    results = measure(captures, a.replay, a.jobs)
    baseline = None
    if a.baseline and os.path.exists(a.baseline) and not a.write_baseline:
        with open(a.baseline) as f:
            baseline = json.load(f)["captures"]
    print_table(results, baseline)

    if a.baseline and (a.write_baseline or not os.path.exists(a.baseline)):
        os.makedirs(os.path.dirname(os.path.abspath(a.baseline)), exist_ok=True)
        with open(a.baseline, "w") as f:
            json.dump({"replay": a.replay, "set": a.set, "captures": results}, f, indent=1)
        print(f"wrote {a.baseline}")
        return 0
    if baseline is not None:
        worse = regressions(results, baseline)
        if worse:
            print(f"worse than {a.baseline} by more than {REGRESSION}:")
            for w in worse:
                print("  " + w)
            return 1
        print(f"no capture's mean worse than {a.baseline} by more than {REGRESSION}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
