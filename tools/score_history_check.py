"""score_history_check: plays a song through the test harness and checks that
score history (src/History/score_history.h) keeps one run for it, with the
results the game shows.

A harness session keeps history only in test_score_history's file, autoplay
runs included (it plays on autoplay), so launch with one and boot a fresh
profile:

  python tools/band3ctl.py launch --fresh -- --test_score_history=out/test_score_history.jsonl
  python tools/band3ctl.py run tests/game/boot.b3t
  python tools/score_history_check.py out/test_score_history.jsonl

It plays whichever song the list is on (guitar, expert, autoplay) at double
speed, to be quick, and waits for the results screen. Then the file must have
one more line than before: a guitar run on expert, marked autoplay, at 200%
speed, with the score the scoreboard ended on and 0-6 stars. Exits 1 when
it doesn't.
"""

import argparse
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import band3ctl  # noqa: E402


def run(conn, commands):
    result = band3ctl.run_script(conn, list(enumerate(commands, start=1)), "score_history_check")
    if not result.passed:
        sys.exit(f"failed at '{result.command}': {result.reply}")


def lines(path):
    if not os.path.isfile(path):
        return []
    with open(path, encoding="utf-8") as f:
        return [line for line in f.read().split("\n") if line]


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


def play_song(conn, timeout):
    """Play Now, Quickplay, Choose Songs, the song the list is on, guitar,
    expert, on autoplay at double speed, to its results."""
    run(conn, ["set autoplay true", "set song_speed 2",
               "press green", "sleep 1s", "press green", "sleep 1s",
               "press green until screen=song_select_screen timeout=30s", "sleep 1500ms",
               "press green until screen=part_difficulty_screen timeout=30s", "sleep 1500ms",
               "press green", "sleep 500ms", "press green", "wait in_game timeout=60s",
               f"wait screen=coop_endgame_screen timeout={timeout}s"])


def main(argv):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("history", help="the test_score_history file band3 was launched with")
    p.add_argument("--port", type=int, default=band3ctl.DEFAULT_PORT)
    p.add_argument("--timeout", type=float, default=600, help="seconds for the song to end")
    a = p.parse_args(argv)

    conn = band3ctl.connect(a.port)
    conn.sock.settimeout(a.timeout + 60)
    before = len(lines(a.history))
    to_main_menu(conn)
    play_song(conn, a.timeout)
    score = conn.command("state").get("state", {}).get("score")
    # the store writes on a thread of its own, after the song
    deadline = time.monotonic() + 10
    while len(lines(a.history)) == before and time.monotonic() < deadline:
        time.sleep(0.25)
    run(conn, ["set autoplay false", "set song_speed 1"])

    new = lines(a.history)[before:]
    print(f"{len(new)} new run(s) in {a.history}")
    for line in new:
        print("  " + line)
    if len(new) != 1:
        print("FAIL: expected one run")
        return 1
    run_ = json.loads(new[0])
    problems = []
    if run_.get("part") != "guitar" or run_.get("diff") != 3:
        problems.append(f"part {run_.get('part')} {run_.get('diff')}, not guitar 3")
    if not run_.get("autoplay"):
        problems.append("not marked autoplay")
    if run_.get("speed") != 200:
        problems.append(f"speed {run_.get('speed')}, not 200")
    if not 0 <= run_.get("stars", -1) <= 6:
        problems.append(f"stars {run_.get('stars')}")
    if not 0 <= run_.get("pct", -1) <= 100:
        problems.append(f"pct {run_.get('pct')}")
    if score is not None and run_.get("score") != score:
        problems.append(f"score {run_.get('score')}, the scoreboard's {score}")
    if run_.get("band_size") != 1:
        problems.append(f"band_size {run_.get('band_size')}")
    for problem in problems:
        print("FAIL: " + problem)
    if not problems:
        print("OK")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
