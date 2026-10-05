"""capdiff: two runs' captures of the same moments, set against each other by what they hold.

The native-only renderer's checks (out/research/n7_3_design.md) run the render
scripts twice: R, the reference, on the emulated GPU (--renderer=emulated), and
N with --emulated_gpu=off, each into its own directory of the harness's
`capture` files. Their pictures differ by more than the renderer (two runs are
two moments), but what the game drew shouldn't: the texture passes, their
targets and sizes, the draws, the display's gamma ramp the sync-only GPU read
from the game's registers (the emulated GPU's plugin reads it in R). For each
name with a <name>.cap in both directories, this runs native_view_replay's
--list on both and compares:

  passes      the pass list: each pass's target kind (texture or back buffer),
              its render-target kind, size, mips, name and whether it was
              carried in from an earlier frame, in order. Not the targets'
              addresses or versions, which differ from run to run
  draws       each pass's draws and the capture's: equal on a still screen,
              within 10% (the capture's) on a moving one
  rt          render targets sampled, missing (no pass made them) and filtered
              (their pass's draws all left out)
  skipped     the draws left out, by why
  post        what post-processing was set to do (post:, addresses left out),
              and the check: line against the composite's constants
  gamma       the gamma: line, exactly

and, with a run.log in both directories (the `band3ctl run` output, which
prints each capture's reply), the replies' rt_missing, rt_filtered, proc_cmds,
composed, the world frame's distance from the game frame, skipped_pass and
skipped_shadow.

A screen is moving when its reference picture moved by itself: R/<name>.png
against R/<name>-again.png (the render scripts take one after each capture)
has a 32x32 block whose mean differs by more than --motion (2, pairs.py
--still's), or --moving names it. Without the -again picture it's still.

It exits 1 when any name's pass list, gamma line or rt_missing differs, or a
draw count does by more than allowed; post:, check: and the other reply
fields are reported but don't fail it.

  python tools/capdiff.py out/n7/ab/menus/N out/n7/ab/menus/R
  python tools/capdiff.py N R screen-song screen-pause      only these
  python tools/capdiff.py N R --moving screen-song,screen-mp2-10s
  python tools/capdiff.py N R --json capdiff.json

Standard library only.
"""

import argparse
import difflib
import glob
import json
import os
import re
import struct
import subprocess
import sys
import tempfile
import zlib
from concurrent.futures import ThreadPoolExecutor
from operator import sub

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_REPLAY = os.path.join(ROOT, "out", "native_view_replay.exe")

MOVING_DRAWS = 0.10
MOTION_LIMIT = 2.0
BLOCK = 32

HEADER_RE = re.compile(r"^frame (\d+), (\d+) draws \((\d+) of them meshes to the back buffer\), "
                       r"(\d+) cameras")
PASSES_RE = re.compile(
    r"^passes: (\d+) \((\d+) into textures, (\d+) of them carried from earlier frames\); "
    r"game frame (\d+); render targets sampled (\d+), missing (\d+), their pass's draws all "
    r"left out (\d+); snapshots (\d+); empty passes left out (\d+), unbalanced (\d+)")
SKIPPED_RE = re.compile(r"(skipped_\w+) (\d+)")
POSTPROC_RE = re.compile(r"^post-processing: from draw (\d+), proc_cmds (-?\d+)")
PASS_RE = re.compile(
    r"^pass +(\d+) (?:back buffer draws (\d+)\.\.(\d+)(.*)"
    r"|texture ([0-9A-Fa-f]+) (\S+) (\d+)x(\d+) mips (\d+), draws (\d+)\.\.(\d+) .*?"
    r"version (\d+), frame (\d+)( \(carried\))?: (.*))$")
# a pass's draws by draw mode, after its name: " mode 1 x12 (cull 6, SKINNED)"
MODES_RE = re.compile(r" mode (\d+) x\d+ \(")
HEX_RE = re.compile(r"\b[0-9A-F]{8}\b")

# the capture reply's fields set side by side, and how
REPLY_FIELDS = ("rt_missing", "rt_filtered", "proc_cmds", "composed", "skipped_pass",
                "skipped_shadow", "held_fallback")


def parse_list(text):
    """What native_view_replay --list printed about a capture: its counts, the
    post:/check:/gamma: lines and the pass list (draw lines aren't kept)."""
    out = {"passes": [], "missing_rts": [], "skipped": {}}
    for line in text.splitlines():
        m = HEADER_RE.match(line)
        if m:
            out.update(frame=int(m[1]), draws=int(m[2]), back_buffer_meshes=int(m[3]),
                       cameras=int(m[4]))
            continue
        m = PASSES_RE.match(line)
        if m:
            g = [int(v) for v in m.groups()]
            out.update(pass_count=g[0], texture_passes=g[1], passes_carried=g[2],
                       game_frame=g[3], rt_sampled=g[4], rt_missing=g[5], rt_filtered=g[6],
                       snapshots=g[7], empty_passes=g[8], unbalanced=g[9])
            continue
        if line.startswith("draws left out:"):
            out["skipped"] = {k: int(v) for k, v in SKIPPED_RE.findall(line)}
            continue
        if line.startswith("  no pass for "):
            out["missing_rts"].append(line.strip())
            continue
        m = POSTPROC_RE.match(line)
        if m:
            out.update(post_boundary=int(m[1]), proc_cmds=int(m[2]))
            continue
        if line.startswith("composed: "):
            out["composed"] = not line.startswith("composed: no")
            continue
        for key in ("post", "check", "gamma"):
            if line.startswith(key + ": "):
                out[key] = line[len(key) + 2:]
        m = PASS_RE.match(line)
        if m:
            if m[2] is not None:
                out["passes"].append({"index": int(m[1]), "target": "back buffer",
                                      "draws": int(m[3]) - int(m[2]),
                                      "modes": MODES_RE.findall(m[4])})
            else:
                tail = m[15]
                cut = MODES_RE.search(tail)
                out["passes"].append({
                    "index": int(m[1]), "target": "texture", "address": m[5], "kind": m[6],
                    "size": f"{m[7]}x{m[8]}", "mips": int(m[9]),
                    "draws": int(m[11]) - int(m[10]), "version": int(m[12]),
                    "frame": int(m[13]), "carried": bool(m[14]),
                    "name": (tail[:cut.start()] if cut else tail).strip(),
                    "modes": MODES_RE.findall(tail)})
    if "draws" not in out or "pass_count" not in out:
        raise ValueError("no capture header in the --list output (a replay from before it?)")
    return out


def pass_key(p):
    """A pass as two runs can agree on it: not its target's address, version or frame."""
    if p["target"] == "back buffer":
        return "back buffer"
    return (f"texture {p['kind']} {p['size']} mips {p['mips']}"
            f"{' carried' if p['carried'] else ''}: {p['name']}")


def without_addresses(line):
    return HEX_RE.sub("-", line or "")


def parse_run_log(text):
    """name -> the reply of each `capture` the log has: `band3ctl run` prints
    them as `<script>:<line>: capture <name>: {json}`, and `band3ctl capture`
    as the JSON alone. The name is the .cap's file name; a later capture of the
    same name wins, as its files did."""
    replies = {}
    for line in text.splitlines():
        start = line.find("{")
        if start < 0 or '"capture"' not in line:
            continue
        try:
            reply = json.loads(line[start:])
        except ValueError:
            continue
        if not isinstance(reply, dict) or not reply.get("capture"):
            continue
        name = os.path.basename(reply["capture"].replace("\\", "/"))
        if name.endswith(".cap"):
            name = name[:-4]
        replies[name] = reply
    return replies


def read_png(path):
    """(width, height, rows of RGB bytes) of an 8-bit RGB or RGBA PNG, without
    a library: band3's are stored (uncompressed) with no filter, but any
    non-interlaced 8-bit one reads."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError(f"{path} isn't a PNG")
    pos, idat, ihdr = 8, [], None
    while pos + 8 <= len(data):
        length, kind = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif kind == b"IDAT":
            idat.append(body)
        elif kind == b"IEND":
            break
    if not ihdr:
        raise ValueError(f"{path} has no IHDR")
    width, height, depth, colour, _, _, interlace = ihdr
    if depth != 8 or colour not in (2, 6) or interlace:
        raise ValueError(f"{path}: only 8-bit RGB/RGBA non-interlaced PNGs read")
    bpp = 3 if colour == 2 else 4
    stride = width * bpp
    raw = zlib.decompress(b"".join(idat))
    prev = bytearray(stride)
    rows = []
    for y in range(height):
        at = y * (stride + 1)
        kind = raw[at]
        line = bytearray(raw[at + 1:at + 1 + stride])
        if kind == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 255
        elif kind == 2:
            line = bytearray((a + b) & 255 for a, b in zip(line, prev))
        elif kind == 3:
            for x in range(stride):
                left = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((left + prev[x]) >> 1)) & 255
        elif kind == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        elif kind != 0:
            raise ValueError(f"{path}: row {y} has filter {kind}")
        prev = line
        rgb = bytearray(line)
        if bpp == 4:
            del rgb[3::4]
        rows.append(bytes(rgb))
    return width, height, rows


def motion(a_path, b_path):
    """The largest 32x32 block mean of |a - b| over RGB (pairs.py's still_mask
    measure), or None if the two aren't the same size."""
    wa, ha, a = read_png(a_path)
    wb, hb, b = read_png(b_path)
    if (wa, ha) != (wb, hb):
        return None
    bw, bh = wa // BLOCK, ha // BLOCK
    sums = [[0] * bw for _ in range(bh)]
    span = BLOCK * 3
    for y in range(bh * BLOCK):
        row = sums[y // BLOCK]
        ra, rb = a[y], b[y]
        for bx in range(bw):
            x0 = bx * span
            row[bx] += sum(map(abs, map(sub, ra[x0:x0 + span], rb[x0:x0 + span])))
    return max((s for row in sums for s in row), default=0) / (BLOCK * BLOCK * 3)


def run_list(replay, cap):
    """--list's output for one capture, or raises with replay's last line."""
    with tempfile.TemporaryDirectory(prefix="capdiff") as tmp:
        r = subprocess.run([replay, cap, os.path.join(tmp, "out.png"), "--list"],
                           capture_output=True, text=True)
    if r.returncode != 0:
        last = (r.stderr or r.stdout).strip().splitlines()[-1:] or ["failed"]
        raise RuntimeError(f"replay failed on {cap}: {last[0]}")
    return r.stdout


def draws_differ(r, n, moving):
    """A failure string if draw counts r and n differ by more than allowed."""
    if r == n:
        return None
    if moving and abs(n - r) <= MOVING_DRAWS * max(r, 1):
        return None
    return f"{r} vs {n}"


def compare(r, n, moving, r_reply=None, n_reply=None):
    """(failures, notes): what fails the name and what's only different."""
    fails, notes = [], []
    rk = [pass_key(p) for p in r["passes"]]
    nk = [pass_key(p) for p in n["passes"]]
    if rk != nk:
        diff = [("R only: " if d[0] == "-" else "N only: ") + d[1:]
                for d in difflib.unified_diff(rk, nk, "R", "N", n=0, lineterm="")
                if not d.startswith(("---", "+++", "@@"))]
        fails.append(f"pass list: {len(rk)} vs {len(nk)} passes: " + "; ".join(diff[:6])
                     + (" ..." if len(diff) > 6 else ""))
    for key in ("pass_count", "texture_passes", "passes_carried"):
        if r.get(key) != n.get(key) and rk == nk:
            fails.append(f"{key} {r.get(key)} vs {n.get(key)}")
    total = draws_differ(r["draws"], n["draws"], moving)
    if total:
        fails.append(f"draws {total}" + (" (more than 10%)" if moving else ""))
    elif r["draws"] != n["draws"]:
        notes.append(f"draws {r['draws']} vs {n['draws']} (moving: within 10%)")
    if rk == nk:
        per_pass, modes = [], []
        for i, (pr, pn) in enumerate(zip(r["passes"], n["passes"])):
            if pr["draws"] != pn["draws"]:
                per_pass.append(f"pass {i} {pr['draws']} vs {pn['draws']}")
            if pr["modes"] != pn["modes"]:
                modes.append(f"pass {i} {' '.join(pr['modes']) or '-'} vs "
                             f"{' '.join(pn['modes']) or '-'}")
        if modes:
            notes.append("draw modes by pass: " + ", ".join(modes[:8])
                         + (" ..." if len(modes) > 8 else ""))
        if per_pass:
            (notes if moving else fails).append("draws by pass: " + ", ".join(per_pass[:8])
                                                + (" ..." if len(per_pass) > 8 else ""))
    if r.get("rt_missing") != n.get("rt_missing"):
        fails.append(f"rt_missing {r.get('rt_missing')} vs {n.get('rt_missing')}"
                     + (": " + "; ".join(n["missing_rts"]) if n["missing_rts"] else ""))
    for key in ("rt_sampled", "rt_filtered", "back_buffer_meshes", "cameras", "proc_cmds",
                "post_boundary", "composed", "snapshots", "empty_passes", "unbalanced"):
        if r.get(key) != n.get(key):
            notes.append(f"{key} {r.get(key)} vs {n.get(key)}")
    for key in sorted(set(r["skipped"]) | set(n["skipped"])):
        if r["skipped"].get(key) != n["skipped"].get(key):
            notes.append(f"{key} {r['skipped'].get(key)} vs {n['skipped'].get(key)}")
    if r.get("gamma") != n.get("gamma"):
        fails.append(f"gamma: R {r.get('gamma')!r} vs N {n.get('gamma')!r}")
    for key in ("post", "check"):
        if without_addresses(r.get(key)) != without_addresses(n.get(key)):
            notes.append(f"{key}: differs")
    if r_reply is not None and n_reply is not None:
        for key in REPLY_FIELDS:
            if r_reply.get(key) != n_reply.get(key):
                text = f"reply {key} {r_reply.get(key)} vs {n_reply.get(key)}"
                (fails if key == "rt_missing" else notes).append(text)
        gap = [x.get("game_frame", 0) - x.get("world_frame", 0) if x.get("composed") else None
               for x in (r_reply, n_reply)]
        if gap[0] != gap[1]:
            notes.append(f"reply world frame {gap[0]} vs {gap[1]} before the game frame")
    return fails, notes


def classify(r_dir, name, moving_names, limit):
    """('moving' or 'still', why)."""
    if name in moving_names:
        return "moving", "--moving"
    shot, again = (os.path.join(r_dir, f"{name}{s}.png") for s in ("", "-again"))
    if not (os.path.exists(shot) and os.path.exists(again)):
        return "still", "no -again shot"
    try:
        m = motion(shot, again)
    except (OSError, ValueError, zlib.error) as e:
        return "still", f"-again unreadable: {e}"
    if m is None:
        return "still", "-again another size"
    return ("moving" if m > limit else "still"), f"-again block mean up to {m:.1f}"


def read_log(directory):
    path = os.path.join(directory, "run.log")
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8", errors="replace") as f:
        return parse_run_log(f.read())


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("n_dir", help="the run to check (native-only): <name>.cap [+ run.log]")
    ap.add_argument("r_dir", help="the reference run: <name>.cap, <name>.png, "
                                  "<name>-again.png [+ run.log]")
    ap.add_argument("names", nargs="*", help="only these (default: every name in both)")
    ap.add_argument("--replay", default=DEFAULT_REPLAY, help="native_view_replay executable")
    ap.add_argument("--moving", action="append", default=[], metavar="NAME[,NAME]",
                    help="these are moving screens whatever their -again shot says "
                         "(draws within 10%%)")
    ap.add_argument("--motion", type=float, default=MOTION_LIMIT, metavar="MEAN",
                    help="a 32x32 block of R/<name>.png against R/<name>-again.png over this "
                         "mean makes the screen moving (default %(default)s)")
    ap.add_argument("--jobs", type=int, default=4, help="replay runs at once")
    ap.add_argument("--json", help="write each name's result here too")
    a = ap.parse_args()

    if not os.path.exists(a.replay):
        sys.exit(f"no replay at {a.replay} (build it: tools/native_view_replay/replay.cpp)")
    caps = lambda d: {os.path.basename(p)[:-4] for p in glob.glob(os.path.join(d, "*.cap"))}
    n_caps, r_caps = caps(a.n_dir), caps(a.r_dir)
    names = sorted(n_caps & r_caps)
    if a.names:
        names = [x for x in names if x in a.names]
    only = sorted((n_caps ^ r_caps) - set(names)) if not a.names else []
    if not names:
        sys.exit(f"no capture in both {a.n_dir} and {a.r_dir}")
    moving_names = {x for arg in a.moving for x in arg.split(",") if x}

    jobs = [(name, d) for name in names for d in (a.r_dir, a.n_dir)]
    with ThreadPoolExecutor(max_workers=a.jobs) as pool:
        lists = dict(zip(jobs, pool.map(
            lambda j: run_list(a.replay, os.path.join(j[1], j[0] + ".cap")), jobs)))
    r_log, n_log = read_log(a.r_dir), read_log(a.n_dir)
    if (r_log is None) != (n_log is None):
        print(f"run.log in only one of the two: the capture replies aren't compared")

    results, failed = {}, []
    for name in names:
        r, n = parse_list(lists[(name, a.r_dir)]), parse_list(lists[(name, a.n_dir)])
        replies = (r_log.get(name), n_log.get(name)) if r_log is not None and \
            n_log is not None else (None, None)
        # whether it moved matters only to draw counts that differ
        moving, why = "still", "draws equal"
        if r["draws"] != n["draws"] or any(
                pr["draws"] != pn["draws"] for pr, pn in zip(r["passes"], n["passes"])):
            moving, why = classify(a.r_dir, name, moving_names, a.motion)
        fails, notes = compare(r, n, moving == "moving", *replies)
        if r_log is not None and n_log is not None and None in replies:
            notes.append("no capture reply in " + " and ".join(
                d for d, x in zip(("R's run.log", "N's run.log"), replies) if x is None))
        status = "different" if fails else ("equal" if not notes else "equal*")
        print(f"{name:28} {moving:6} {status:9} passes {len(r['passes'])}/{len(n['passes'])} "
              f"draws {r['draws']}/{n['draws']} rt_missing {r['rt_missing']}/{n['rt_missing']}"
              f"  ({why})")
        for f in fails:
            print(f"    FAIL {f}")
        for note in notes:
            print(f"    note {note}")
        if fails:
            failed.append(name)
        results[name] = {"moving": moving, "why": why, "fails": fails, "notes": notes,
                         "draws": [r["draws"], n["draws"]],
                         "passes": [len(r["passes"]), len(n["passes"])],
                         "rt_missing": [r["rt_missing"], n["rt_missing"]]}
    print()
    if only:
        print(f"in one directory only: {', '.join(only)}")
    print(f"{len(names) - len(failed)} of {len(names)} the same (equal* has notes)"
          + (f"; different: {', '.join(failed)}" if failed else ""))
    if a.json:
        with open(a.json, "w") as f:
            json.dump(results, f, indent=1)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
