"""LAUNCHES THE GAME: band3 lighting Stage Kits itself, through a song.

python tools/test_stagekit_lights.py   (a harness test: it needs the game built, and
starts band3 minimized and muted through band3ctl, so ask before running it on a
machine someone is using)

It starts tools/fake_pico.py's wireless Stage Kit, sending its telemetry to
127.0.0.1 and taking Stage Kit events on 127.0.0.1:21070, then band3 on a fresh
test profile (out/test_user_data_lights) with

  --stagekit_fake=true --events_enabled=true --events_target=127.0.0.1

and checks, in order:

  - the harness's lights lists the pretend USB kit and the fake Pico, with no
    problem, and the pretend kit is sent all-off first
  - lights_test reaches the device it names and no other: red LEDs 0x55 to the
    pretend kit, fog on to the Pico; then all off to every device, which the
    pretend kit gets as what changed (red LEDs 0x00) and the Pico as all-off
  - through tests/game/boot.b3t and the menus, 20th Century Boy on autoplay: the
    game's Stage Kit commands reach the pretend kit and the Pico (through the
    RB3Enhanced events)
  - back to the music library, the pretend kit's lights end all off
  - band3 quit through the harness sends the Pico all-off

band3 under the harness broadcasts no discovery, so the Picos on the network are
left alone. band3's log goes to out/<name>.log (--name, test_stagekit_lights by
default). Exits 0 when every check passes, 1 otherwise; band3 is killed and the
fake Pico stopped either way.

Standard library only.
"""

import argparse
import json
import os
import subprocess
import sys
import time

import band3ctl
import fake_pico
from test_home_assistant import (Checks, Failed, LEAVE_SONG, PAUSE, TO_SONG, band3ctl_command,
                                 kill, script, wait_for_exit)

REPO = band3ctl.REPO
OUT = os.path.join(REPO, "out")
FAKE = "usb:fake"
PICO = "pico:127.0.0.1"

KINDS = {0x20: "blue", 0x40: "green", 0x60: "yellow", 0x80: "red"}


def apply(state, left, right):
    """One Stage Kit command applied, as src/Lights/stagekit.cpp's ApplyStageKit."""
    state = dict(state)
    if right in KINDS:
        state[KINDS[right]] = left
    elif right == 0x01:
        state["fog"] = True
    elif right == 0x02:
        state["fog"] = False
    elif 0x03 <= right <= 0x06:
        state["strobe"] = right - 0x02
    elif right == 0x07:
        state["strobe"] = 0
    elif right == 0xFF:
        state = all_off()
    return state


def all_off():
    return {"red": 0, "yellow": 0, "green": 0, "blue": 0, "strobe": 0, "fog": False}


class Lights:
    """The harness's lights, with the pretend kit's commands kept as they're taken."""

    def __init__(self, conn):
        self.conn = conn
        self.fake = []

    def poll(self):
        reply = self.conn.command("lights")
        lights = reply.get("lights", {})
        self.fake += [tuple(c) for c in lights.get("fake", [])]
        return lights

    def wait_for(self, test, timeout):
        deadline = time.time() + timeout
        while True:
            lights = self.poll()
            value = test(lights)
            if value or time.time() >= deadline:
                return value or None
            time.sleep(0.2)

    def fake_state(self):
        state = all_off()
        for left, right in self.fake:
            state = apply(state, left, right)
        return state


def run(args, pico, checks, log_path):
    launch = band3ctl_command(
        "--port", str(args.port), "launch", "--fresh", "--exe", args.exe,
        "--user-data", args.user_data, "--",
        "--stagekit_fake=true", "--stagekit_usb=true", "--pico_discovery=true",
        "--events_enabled=true", "--events_target=127.0.0.1", f"--log_file={log_path}")
    print(" ".join(launch), flush=True)
    launched = subprocess.run(launch, capture_output=True, text=True, cwd=REPO)
    pid = None
    if launched.returncode == 0:
        try:
            pid = json.loads(launched.stdout.strip().splitlines()[-1])["pid"]
        except (ValueError, KeyError, IndexError):
            pass
    args.pid = pid
    checks.require("band3 starts under the harness", pid is not None,
                   (launched.stdout + launched.stderr).strip())

    conn = band3ctl.connect(args.port, wait_s=10)
    try:
        lights = Lights(conn)

        def keys(view):
            return {d["key"] for d in view.get("devices", [])}

        found = lights.wait_for(lambda v: {FAKE, PICO} <= keys(v) and v, 20) or lights.poll()
        checks.require("lights lists the pretend kit and the fake Pico", {FAKE, PICO} <= keys(found),
                       json.dumps(found))
        checks.check("no problem finding Picos", found.get("problem") == "", found.get("problem"))
        pico_row = next(d for d in found["devices"] if d["key"] == PICO)
        checks.check("the Pico's row names it and its kit",
                     pico_row["name"] == pico.status["name"] and "kit: connected" in pico_row["detail"]
                     and pico_row["online"], json.dumps(pico_row))
        lights.wait_for(lambda v: lights.fake, 5)
        checks.check("the pretend kit is sent all-off first",
                     lights.fake[:1] == [(0x00, 0xFF)], str(lights.fake))

        # a test command for one device
        lights.fake.clear()
        since = time.time()
        conn.command(f"lights_test 0x55 0x80 {FAKE}")
        checks.check("lights_test reaches the pretend kit",
                     lights.wait_for(lambda v: (0x55, 0x80) in lights.fake, 5),
                     str(lights.fake))
        conn.command(f"lights_test 0 1 {PICO}")
        got = pico.wait_for(lambda: [(e.left, e.right) for e in pico.events(since)], 5) or []
        checks.check("lights_test reaches the Pico, and only what was sent to it",
                     got == [(0, 1)], str(got))
        lights.poll()
        checks.check("the pretend kit isn't sent the Pico's command",
                     lights.fake == [(0x55, 0x80)], str(lights.fake))

        # and for every device
        lights.fake.clear()
        since = time.time()
        conn.command("lights_test 0 0xFF")
        checks.check("all off reaches the pretend kit as what changed",
                     lights.wait_for(lambda v: (0x00, 0x80) in lights.fake, 5), str(lights.fake))
        got = pico.wait_for(lambda: [(e.left, e.right) for e in pico.events(since)], 5) or []
        checks.check("all off reaches the Pico", (0, 0xFF) in got, str(got))

        # the game's lights
        with open(os.path.join(REPO, "tests", "game", "boot.b3t"), encoding="utf-8") as f:
            script(conn, f.read(), "boot", checks)
        lights.poll()
        lights.fake.clear()
        script(conn, TO_SONG, "to the song", checks)
        in_song = time.time()
        checks.check("the game's commands reach the pretend kit in the song",
                     lights.wait_for(lambda v: lights.fake, 60), "none in 60 s")
        checks.check("the game's commands reach the Pico in the song",
                     pico.wait_for(lambda: pico.events(in_song), 10), "none")
        print(f"     the pretend kit was sent {len(lights.fake)} commands; "
              f"the Pico {len(pico.events(in_song))} events", flush=True)

        script(conn, PAUSE, "pause", checks)
        script(conn, LEAVE_SONG, "back to the music library", checks)
        lights.wait_for(lambda v: lights.fake_state() == all_off(), 10)
        checks.check("the pretend kit's lights end all off after the song",
                     lights.fake_state() == all_off(), json.dumps(lights.fake_state()))

        # quitting turns the Pico off
        since = time.time()
        conn.command("quit")
    finally:
        conn.close()
    got = pico.wait_for(lambda: [(e.left, e.right) for e in pico.events(since)
                                 if (e.left, e.right) == (0, 0xFF)], 15)
    checks.check("quitting sends the Pico all-off", got, "nothing")
    wait_for_exit(pid, 30)
    args.pid = None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", default=band3ctl.DEFAULT_EXE)
    parser.add_argument("--port", type=int, default=band3ctl.DEFAULT_PORT,
                        help="the harness port")
    parser.add_argument("--user-data", default=os.path.join(OUT, "test_user_data_lights"),
                        help="the test profile, emptied first")
    parser.add_argument("--name", default="test_stagekit_lights",
                        help="the log's name, under out/")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.exe):
        sys.exit(f"no band3 at {args.exe}")
    try:
        band3ctl.Connection(args.port).close()
        sys.exit(f"a band3 already answers on harness port {args.port}; quit it or pick --port")
    except OSError:
        pass
    os.makedirs(OUT, exist_ok=True)
    log_path = os.path.join(OUT, f"{args.name}.log")

    try:
        pico = fake_pico.FakePico().start()
    except OSError as e:
        sys.exit(f"the fake Pico can't take UDP {fake_pico.RB3E_PORT} on 127.0.0.1: {e}")
    checks = Checks()
    args.pid = None
    try:
        run(args, pico, checks, log_path)
    except Failed:
        pass
    except OSError as e:
        checks.check("the harness answers", False, str(e))
    finally:
        if args.pid:
            kill(args.pid)
            wait_for_exit(args.pid)
        pico.stop()

    if checks.failed:
        print(f"FAIL {len(checks.failed)} of the checks")
        print(f"band3's Lights: log lines ({log_path}):")
        try:
            with open(log_path, encoding="utf-8", errors="replace") as f:
                for line in [l.rstrip() for l in f if "Lights:" in l][-40:]:
                    print(f"  {line}")
        except OSError:
            print(f"  (no log at {log_path})")
        return 1
    print("PASS band3 lights the pretend USB Stage Kit and the fake Pico, and tests both")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
