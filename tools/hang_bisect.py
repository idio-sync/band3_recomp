"""Bisects the GPU hang at refresh_rate 120: boots band3 and plays a song
(tests/game/boot.b3t, then pacing_song.b3t) under each setting that takes one
of the renderers' interactions away, a few times each, and counts the runs
that hung (DEVICE_HUNG or a removed device in the log).

  baseline       renderer native, as it hangs
  no-zero-copy   native_present_zero_copy off: frames read back and uploaded,
                 so none crosses from SDL_gpu's queue to the SDK's and band3
                 binds nothing in the SDK's command list
  emulated-full  emulated_gpu_while_native full: the emulated GPU draws
                 everything, not the stream skip_draws leaves it
  emulated       renderer emulated: the native renderer idle

The hang can take the whole machine down, so each run is written to the
journal (flushed to disk) as it starts and as it ends. Run the script again
after a reboot: a run that started and never ended is recorded as having
taken the machine down, and the rest carry on from there. Rounds go through
every setting once before the next round, so a short session still covers
them all.

  python tools/hang_bisect.py                    # 3 rounds
  python tools/hang_bisect.py --rounds 5 --only baseline,no-zero-copy
  python tools/hang_bisect.py --summary          # the journal's counts so far
"""

import argparse
import json
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BAND3CTL = os.path.join(REPO, "tools", "band3ctl.py")
LOGS = os.path.join(REPO, "out", "build", "win-amd64-release", "logs")
JOURNAL = os.path.join(REPO, "out", "hang_bisect.jsonl")
COMMON = ["--video_mode_refresh_rate=120", "--test_random_seed=21"]
CONFIGS = {
    "baseline": ["--renderer=native"],
    "no-zero-copy": ["--renderer=native", "--native_present_zero_copy=false"],
    "emulated-full": ["--renderer=native", "--emulated_gpu_while_native=full"],
    "emulated": ["--renderer=emulated"],
}
HANG_MARKERS = ("DEVICE_HUNG", "device removed", "Device removed")


def append(journal, entry):
    entry["time"] = time.strftime("%Y-%m-%d %H:%M:%S")
    with open(journal, "a", encoding="utf-8") as f:
        f.write(json.dumps(entry) + "\n")
        f.flush()
        os.fsync(f.fileno())


def read(journal):
    if not os.path.exists(journal):
        return []
    with open(journal, encoding="utf-8") as f:
        return [json.loads(line) for line in f if line.strip()]


def ctl(*args, timeout=900):
    try:
        return subprocess.run([sys.executable, BAND3CTL, *args], cwd=REPO, timeout=timeout,
                              capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return None


def kill_band3():
    if os.name == "nt":
        subprocess.run(["taskkill", "/IM", "band3.exe", "/F"], capture_output=True)
    else:
        subprocess.run(["pkill", "-x", "band3"], capture_output=True)


def logs():
    return set(os.listdir(LOGS)) if os.path.isdir(LOGS) else set()


def hang_line(names):
    for name in sorted(names):
        with open(os.path.join(LOGS, name), encoding="utf-8", errors="replace") as f:
            for line in f:
                if any(m in line for m in HANG_MARKERS):
                    return line.strip()[:300]
    return None


def one_run(config):
    before = logs()
    started = time.monotonic()
    launched = ctl("launch", "--fresh", "--", *COMMON, *CONFIGS[config], timeout=180)
    if not launched or launched.returncode != 0:
        result = {"result": "launch failed"}
    else:
        # a minimized window doesn't paint
        ctl("window", "offscreen", timeout=30)
        result = {"result": "ok"}
        for script in ("tests/game/boot.b3t", "tests/game/pacing_song.b3t"):
            ran = ctl("run", script)
            if not ran or ran.returncode != 0:
                tail = (ran.stdout + ran.stderr).strip().splitlines()[-1:] if ran else ["timed out"]
                result = {"result": "failed", "script": script, "why": tail[0][:300] if tail else ""}
                break
    new = logs() - before
    hung = hang_line(new)
    if hung:
        result = {"result": "hung", "line": hung}
    result["seconds"] = round(time.monotonic() - started, 1)
    result["log"] = ",".join(sorted(new))
    kill_band3()
    # the driver's own recovery after a hang
    time.sleep(10 if hung else 3)
    return result


def summary(entries):
    counts = {}
    for e in entries:
        if e.get("event") != "end":
            continue
        c = counts.setdefault(e["config"], {})
        c[e["result"]] = c.get(e["result"], 0) + 1
    for config in CONFIGS:
        if config in counts:
            parts = ", ".join(f"{n} {r}" for r, n in sorted(counts[config].items()))
            print(f"{config:14} {parts}")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--only", help="comma-separated settings, of " + ", ".join(CONFIGS))
    parser.add_argument("--journal", default=JOURNAL)
    parser.add_argument("--summary", action="store_true", help="print the counts and stop")
    args = parser.parse_args()
    configs = args.only.split(",") if args.only else list(CONFIGS)
    for c in configs:
        if c not in CONFIGS:
            sys.exit(f"no setting {c}; there are {', '.join(CONFIGS)}")

    entries = read(args.journal)
    if args.summary:
        summary(entries)
        return 0
    # a run that started and never ended took the machine down with it
    ended = {(e["config"], e["round"]) for e in entries if e.get("event") == "end"}
    for e in entries:
        if e.get("event") == "start" and (e["config"], e["round"]) not in ended:
            append(args.journal, {"event": "end", "config": e["config"], "round": e["round"],
                                  "result": "machine down", "started": e["time"]})
            ended.add((e["config"], e["round"]))
            print(f"{e['config']} round {e['round']}: machine down (started {e['time']})")

    for rnd in range(1, args.rounds + 1):
        for config in configs:
            if (config, rnd) in ended:
                continue
            append(args.journal, {"event": "start", "config": config, "round": rnd})
            print(f"{config} round {rnd} ...", end=" ", flush=True)
            result = one_run(config)
            append(args.journal, {"event": "end", "config": config, "round": rnd, **result})
            print(result["result"], result.get("line", result.get("why", "")))
    summary(read(args.journal))
    return 0


if __name__ == "__main__":
    sys.exit(main())
