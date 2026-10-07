"""overdrive_check: captures the moments after an overdrive deploy and sets the
native renderer's picture of each against the emulated GPU's.

The native renderer once drew the highway's overdrive flames (a particle system
that moves with the track) without the transform the game draws them through
(RndParticleSys's mRelativeXfm), and they streaked across the venue above the
highway; the emulated GPU, running the game's own shaders, never did. This
checks for that: bright warm pixels above the strikeline in the native picture
that the emulated one hasn't.

band3's own `autoplay` setting never deploys overdrive (an autoplaying player
ignores the deploy buttons), so this turns on Rock Band 3 Deluxe's Autoplay
modifier as well, whose bots deploy it by themselves (dx_bot_funcs.dta: once
each bot's meter is half full). That modifier lasts the session and turns
saving off for it; turning it on sends the game back to the main menu or the
title screen. It's reached through the pause menu: Options, Modifiers, then
seven up from the top (the list wraps; the six below it are RB3Enhanced's,
which band3 adds).

Launch with both renderers, so each `capture` has the emulated GPU's picture
(<name>.png) and the native one (<name>.gpu.png), at 120 Hz as the report was,
and boot a fresh profile:

  python tools/band3ctl.py launch --fresh -- --renderer=both --video_mode_refresh_rate=120 --frame_cap=120 --native_view_record_targets=true --async_shader_compilation=false
  python tools/band3ctl.py run tests/game/boot.b3t
  python tools/overdrive_check.py

It plays whichever song the list is on (guitar, expert, on autoplay, in
arena_04), so the run is whatever song has overdrive soon enough (300 s by
default). A track theme with its own overdrive look goes in the profile before
the launch (without --fresh): dx_track_theme_<name>.dta in
out/test_user_data/game, and (selected_track_theme <name>) in dx_settings.dta
there.

  python tools/overdrive_check.py --modifier-on     Deluxe's Autoplay is already on
  python tools/overdrive_check.py --compare <screenshots> <prefix> <count>
                                                    only the comparison, of captures taken before

Exits 1 when the captures together have more than --limit such pixels (300:
the fixed renderer had 0 to 96 in a run, from small differences elsewhere; the
streaks were 1300 and more), 2 when no deploy was seen. Needs numpy and Pillow.
"""

import argparse
import os
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import band3ctl  # noqa: E402


def gold(path):
    """The share of the highway's lower part in overdrive gold: the deploy's
    flash and the gold rails while it runs."""
    a = np.asarray(Image.open(path).convert("RGB")).astype(int)
    h, w, _ = a.shape
    r = a[int(h * 0.45):int(h * 0.95), int(w * 0.3):int(w * 0.7)]
    return float(((r[..., 0] > 200) & (r[..., 1] > 150) & (r[..., 2] < 90)).mean())


def native_only_warm(emulated, native):
    """Bright warm pixels above the strikeline (the picture's top 55%) the native
    picture has and the emulated one hasn't: (count, bounding box or None)."""
    a = np.asarray(Image.open(emulated).convert("RGB")).astype(int)
    b = np.asarray(Image.open(native).convert("RGB")).astype(int)
    if a.shape != b.shape:
        raise ValueError(f"{emulated} and {native} aren't the same size")
    diff = np.abs(a - b).max(axis=2)
    warm = (b[..., 0] > 180) & (b[..., 0] - b[..., 2] > 100) & (diff > 80)
    warm[int(b.shape[0] * 0.55):] = False
    ys, xs = np.nonzero(warm)
    box = (int(xs.min()), int(ys.min()), int(xs.max()), int(ys.max())) if len(xs) else None
    return int(warm.sum()), box


def compare(pairs, limit):
    total = 0
    for name, emulated, native in pairs:
        n, box = native_only_warm(emulated, native)
        total += n
        print(f"{name}: {n} native-only warm pixels above the strikeline"
              + (f" in {box}" if box else ""))
    print(f"total {total} (limit {limit})")
    return 0 if total <= limit else 1


def run(conn, commands):
    result = band3ctl.run_script(conn, list(enumerate(commands, start=1)), "overdrive_check")
    if not result.passed:
        sys.exit(f"failed at '{result.command}': {result.reply}")


def to_main_menu(conn):
    """From the first-run welcome hint, the title screen or the main menu, to
    the main menu."""
    screen = conn.command("state").get("state", {}).get("screen", "")
    if screen == "hint_rb3_welcome_screen":
        run(conn, ["sleep 1s", "press green until screen=manage_band_screen timeout=30s",
                   "sleep 1s", "press red until screen=main_hub_screen timeout=30s"])
    elif screen != "main_hub_screen":
        run(conn, ["press start until screen=main_hub_screen timeout=60s"])
    run(conn, ["sleep 1500ms"])


def start_song(conn):
    """Play Now, Quickplay, Choose Songs, the song the list is on, guitar, expert."""
    run(conn, ["set autoplay true", "set forced_venue arena_04",
               "press green", "sleep 1s", "press green", "sleep 1s",
               "press green until screen=song_select_screen timeout=30s", "sleep 1500ms",
               "press green until screen=part_difficulty_screen timeout=30s", "sleep 1500ms",
               "press green", "sleep 500ms", "press green", "wait in_game timeout=60s"])


def autoplay_modifier(conn):
    """Deluxe's Autoplay modifier on, from a song just started: presses in its
    first seconds are dropped, so it waits a little first."""
    down = lambda n: [c for _ in range(n) for c in ("press down", "sleep 400ms")]
    up = [c for _ in range(7) for c in ("press up", "sleep 450ms")]
    run(conn, ["sleep 8s", "press start", "sleep 1500ms"] + down(3)
        + ["press green", "sleep 1500ms"] + down(5) + ["press green", "sleep 1500ms"] + up
        # the modifier, then Continue on its warning about saving
        + ["press green", "sleep 1s", "press green", "sleep 4s"])


def catch_deploy(conn, prefix, count, timeout):
    """Screenshots until the highway turns gold, then `count` captures."""
    start = time.monotonic()
    while time.monotonic() - start < timeout:
        reply = conn.command("screenshot overdrive-watch")
        if not reply.get("ok"):
            sys.exit(f"screenshot failed: {reply}")
        g = gold(reply["path"])
        if g > 0.08:
            print(f"deploy seen at {time.monotonic() - start:.1f} s (gold {g:.3f})", flush=True)
            pairs = []
            for i in range(count):
                name = f"{prefix}{i + 1}"
                reply = conn.command(f"capture {name}")
                if not reply.get("ok"):
                    sys.exit(f"capture {name} failed: {reply}")
                if "gpu" not in reply:
                    sys.exit(f"capture {name} has no native picture: {reply.get('gpu_error')}")
                pairs.append((name, reply["path"], reply["gpu"]))
                time.sleep(0.15)
            return pairs
        time.sleep(0.05)
    return None


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--port", type=int, default=band3ctl.DEFAULT_PORT)
    p.add_argument("--prefix", default="overdrive", help="the captures' names, numbered")
    p.add_argument("--captures", type=int, default=14)
    p.add_argument("--timeout", type=float, default=300, help="seconds to wait for a deploy")
    p.add_argument("--limit", type=int, default=300)
    p.add_argument("--modifier-on", action="store_true",
                   help="Deluxe's Autoplay modifier is already on this session")
    p.add_argument("--compare", nargs=3, metavar=("DIR", "PREFIX", "COUNT"))
    a = p.parse_args(argv)

    if a.compare:
        d, prefix, n = a.compare[0], a.compare[1], int(a.compare[2])
        pairs = [(f"{prefix}{i}", os.path.join(d, f"{prefix}{i}.png"),
                  os.path.join(d, f"{prefix}{i}.gpu.png")) for i in range(1, n + 1)]
        return compare(pairs, a.limit)

    conn = band3ctl.connect(a.port)
    conn.sock.settimeout(120)
    to_main_menu(conn)
    if not a.modifier_on:
        start_song(conn)
        autoplay_modifier(conn)
        to_main_menu(conn)
    start_song(conn)
    pairs = catch_deploy(conn, a.prefix, a.captures, a.timeout)
    if pairs is None:
        print(f"no deploy seen in {a.timeout:.0f} s")
        return 2
    return compare(pairs, a.limit)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
