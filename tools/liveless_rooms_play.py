"""liveless_rooms_play: two band3s on this PC play a song together, joined by code.

Runs tests/game/liveless_rooms_host.b3t's procedure in one command: the mock Liveless
Rooms server (tools/liveless_rooms_mock.py, on 127.0.0.1, never the public server),
the host on harness port 21181 and the joiner on 21182 (fresh profiles, minimized and
muted, through band3ctl), both online, then the two scripts at once (the host's waits
for the joiner's join, and each for the other at the song's menus). At the results it
prints each game's band score and fails unless they're the same, then quits both games
and stops the mock, pass or fail.

  python tools/liveless_rooms_play.py                 logs as out/liveless_rooms_play_*.log
  python tools/liveless_rooms_play.py --name run1     logs as out/run1_*.log

Standard library only.
"""

import argparse
import json
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "out")
CTL = [sys.executable, os.path.join(REPO, "tools", "band3ctl.py")]
HOST, JOINER = 21181, 21182
GAME = ["--liveless=true", "--liveless_rooms=true", "--liveless_rooms_server=127.0.0.1",
        "--log_net_calls=true"]


def ctl(port, *args):
    """band3ctl's command for the game on `port`."""
    return CTL + ["--port", str(port), *args]


def check(port, *args):
    """Runs a band3ctl command to the end; its output is shown as it goes."""
    if subprocess.run(ctl(port, *args), cwd=REPO).returncode:
        raise SystemExit(f"failed on {port}: {' '.join(args)}")


def run_both(host_args, joiner_args):
    """The host's and the joiner's commands at once; fails unless both pass."""
    host = subprocess.Popen(ctl(HOST, *host_args), cwd=REPO)
    joiner = subprocess.run(ctl(JOINER, *joiner_args), cwd=REPO).returncode
    if host.wait() or joiner:
        raise SystemExit(f"failed: host {host.returncode}, joiner {joiner}")


def state(port):
    out = subprocess.run(ctl(port, "state"), cwd=REPO, capture_output=True, text=True).stdout
    return json.loads(out)["state"]


def wait_for_mock(log_path, mock, timeout_s=10):
    deadline = time.monotonic() + timeout_s
    while time.monotonic() < deadline:
        if mock.poll() is not None:
            raise SystemExit(f"the mock exited; see {log_path}")
        with open(log_path, encoding="utf-8", errors="replace") as f:
            if "listening on" in f.read():
                return
        time.sleep(0.2)
    raise SystemExit(f"the mock didn't start; see {log_path}")


def play(name):
    log = lambda what: os.path.join(OUT, f"{name}_{what}.log")
    check(HOST, "launch", "--fresh", "--", *GAME, "--username=host",
          f"--log_file={log('host')}")
    # the host logs in first, so the mock gives it HOST0001
    check(HOST, "wait rooms=logged_in timeout=20s")
    check(JOINER, "launch", "--fresh", "--user-data", "out/test_user_data_b", "--", *GAME,
          "--username=joiner", "--liveless_port=9203", f"--log_file={log('joiner')}")
    # both online before the joiner asks for the host's code
    run_both(["run", "tests/game/liveless_online.b3t"], ["run", "tests/game/liveless_online.b3t"])
    run_both(["run", "tests/game/liveless_rooms_host.b3t"],
             ["run", "tests/game/liveless_rooms_join.b3t"])
    host, joiner = state(HOST), state(JOINER)
    for who, s in (("host", host), ("joiner", joiner)):
        players = sum(m["exists"] for m in s["band"])
        print(f"{who}: {s['screen']}, {s['song']['shortname']}, {players} players, "
              f"band score {s['score']}")
    if host["score"] != joiner["score"] or host["score"] <= 0:
        raise SystemExit(f"band scores differ: host {host['score']}, joiner {joiner['score']}")
    print(f"PASS: band score {host['score']} on both")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--name", default="liveless_rooms_play",
                        help="the logs' prefix, under out/")
    args = parser.parse_args()
    os.makedirs(OUT, exist_ok=True)
    mock_log = os.path.join(OUT, f"{args.name}_mock.log")
    with open(mock_log, "w", encoding="utf-8") as f:
        mock = subprocess.Popen(
            [sys.executable, os.path.join(REPO, "tools", "liveless_rooms_mock.py"),
             "--address", "127.0.0.1", "--ping", "2", "--codes", "HOST0001,JOIN0001"],
            cwd=REPO, stdout=f, stderr=subprocess.STDOUT)
    try:
        wait_for_mock(mock_log, mock)
        play(args.name)
    finally:
        for port in (HOST, JOINER):
            subprocess.run(ctl(port, "quit"), cwd=REPO, capture_output=True)
        mock.terminate()
        mock.wait()


if __name__ == "__main__":
    main()
