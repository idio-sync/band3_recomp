"""perf_driver: one measured run of a song, through band3ctl, with every guard.

  python tools/perf_driver.py <60|120> <run> [--exe <band3.exe>] [--seconds N] [--flag <arg>]...
  python tools/perf_driver.py table <run>...

Launches band3 (renderer native) on a fresh test profile, boots it (tests/game/boot.b3t), plays
20th Century Boy on autoplay to 5 s past its loading (tests/game/perf_slice.b3t), then measures
--seconds (30) of it: tools/perf_sample.py from outside, and the harness's `native_view stats`
and `present_stats`. The rate sets --video_mode_refresh_rate and --frame_cap (a
`--flag --frame_cap=...` replaces the cap); --flag adds any band3 argument. Environment:
WINDOW=onscreen puts the window on the primary monitor, unfocused (offscreen by default: off
every monitor there's no real vertical blank, so pacing numbers need onscreen on the machine's
own display); RENDERER_FLAG replaces --renderer=native.

Guards, as a GPU hang can take this machine down: it launches only with the GPU reported OK, the
console session active (Remote Desktop has no real vertical blank) and no other band3 or build
running (it waits up to an hour, every 2 minutes); a log watch quits the game at a hang line or
a worker frame waiting over 250 ms for the GPU; perf_sample's --guard stops a GPU flat out;
after, event 4101 and the adapter's status. A build starting during the run marks it contended
(its timings, not its pictures, are spoiled).

Writes out/perf/<run>/: driver.log, ctl.txt (every band3ctl exchange), perf.jsonl and
result.json (the flags, native_view stats, present_stats, state, contended, hang_signs).
Exit 0 ok, 1 a step failed, 3 hang signs or a guard tripped, 4 preconditions not met.
`table` prints the runs' present_stats side by side: latency, publish latency, frames shown,
repeated and skipped, step p95, judder, the UI thread's round trip and the cap's late frames.

Windows only.
"""
import glob
import json
import os
import re
import subprocess
import sys
import threading
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RUNS = os.path.join(REPO, "out", "perf")
PORT = "21097"
HANG = ("DEVICE_HUNG", "device removed", "DEVICE_REMOVED", "DXGI_ERROR", "TDR", "didn't finish",
        "watchdog", "holding the last frame")
BUILD_PROCS = "ninja,cmake,clang-cl,clang,clang++,cl,link,lld-link,msbuild"
GPU_OK = "OK AMD Radeon AI PRO R9700"


def table(names):
    print(f"{'run':16s} {'lat p50/p95':>13s} {'pub p50':>8s} {'shown/rep/skip':>16s} "
          f"{'step p95':>9s} {'judder':>7s} {'ui p50':>7s} {'cap late/resets':>16s}")
    for name in names:
        try:
            r = json.load(open(os.path.join(RUNS, name, "result.json"), encoding="utf-8"))
        except OSError:
            print(f"{name:16s} (no result)")
            continue
        s = r.get("present", {}).get("stats", {})
        n, g, u = s.get("native", {}), s.get("game", {}), s.get("ui_round_trip", {})
        lat, pub, st = n.get("latency_ms", {}), n.get("publish_latency_ms", {}), n.get("step_ms", {})
        cap = g.get("cap", {})
        flags = (" CONTENDED" if r.get("contended") else "") + (" HANG" if r.get("hang_signs") else "")
        print(f"{name:16s} {lat.get('p50', 0):6.2f}/{lat.get('p95', 0):6.2f} "
              f"{pub.get('p50', 0):8.2f} {n.get('shown')}/{n.get('repeats')}/{n.get('skipped'):<5} "
              f"{st.get('p95', 0):9.2f} {n.get('judder')!s:>7} {u.get('p50', 0):7.2f} "
              f"{cap.get('late')!s:>7}/{cap.get('resets')!s:<7}{flags}")


def ps(command):
    return subprocess.run(["powershell", "-NoProfile", "-Command", command], capture_output=True,
                          text=True).stdout.strip()


class Run:
    def __init__(self, rate, name, exe, seconds, extra):
        self.name, self.exe, self.seconds = name, exe, seconds
        self.build = os.path.dirname(exe)
        self.out = os.path.join(RUNS, name)
        os.makedirs(self.out, exist_ok=True)
        self.logf = open(os.path.join(self.out, "driver.log"), "a", encoding="utf-8")
        self.ud = os.path.join(os.environ.get("TEMP", REPO), "band3_perf_ud")
        self.ctl_cmd = [sys.executable, os.path.join(REPO, "tools", "band3ctl.py"), "--port", PORT]
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
        self.flags = [os.environ.get("RENDERER_FLAG", "--renderer=native"), "--test_random_seed=21",
                      "--async_shader_compilation=false", "--dred=true",
                      "--native_gpu_timestamps=true", f"--video_mode_refresh_rate={rate}"]
        if not any(f.startswith("--frame_cap=") for f in extra):
            self.flags.append(f"--frame_cap={rate}")
        self.flags += extra

    def log(self, msg):
        line = f"{time.strftime('%H:%M:%S')} {msg}"
        print(line, flush=True)
        self.logf.write(line + "\n")
        self.logf.flush()

    def ctl(self, args, timeout=None):
        t0 = time.monotonic()
        p = subprocess.run(self.ctl_cmd + args, cwd=REPO, env=self.env, capture_output=True,
                           text=True, timeout=timeout)
        text = p.stdout + p.stderr
        with open(os.path.join(self.out, "ctl.txt"), "a", encoding="utf-8") as f:
            f.write(f"### {' '.join(args)} (exit {p.returncode}, {time.monotonic() - t0:.1f} s)\n{text}")
        last = text.strip().splitlines()[-1][:200] if text.strip() else ""
        self.log(f"band3ctl {' '.join(args)[:120]} -> exit {p.returncode} in "
                 f"{time.monotonic() - t0:.1f} s; {last}")
        return p.returncode, text

    def busy(self):
        b = ps("Get-Process band3 -EA SilentlyContinue | % { \"$($_.Id) $($_.Path)\" }")
        return b or ps(f"Get-Process {BUILD_PROCS} -EA SilentlyContinue | % {{ \"$($_.Id) $($_.Name)\" }}")

    def preconditions(self):
        gpu = ps("Get-PnpDevice -Class Display -PresentOnly | % { \"$($_.Status) $($_.FriendlyName)\" }")
        if GPU_OK not in gpu:
            self.log(f"the GPU isn't OK ({gpu}): not launching")
            return False
        users = subprocess.run(["query", "user"], capture_output=True, text=True).stdout
        if not re.search(r"\bconsole\s+\d+\s+Active", users, re.I):
            self.log("the console session isn't active (Remote Desktop?): not launching")
            return False
        for i in range(31):
            b = self.busy()
            if not b:
                return True
            self.log(f"busy: {b}" + ("; waiting 2 min" if i < 30 else "; gave up"))
            if i < 30:
                time.sleep(120)
        return False

    def watch(self, path, stop, found):
        pos = 0
        while not stop.is_set():
            try:
                with open(path, encoding="utf-8", errors="replace") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
            except OSError:
                chunk = ""
            for line in chunk.splitlines():
                m = re.search(r"([\d.]+) of it waiting for the GPU", line)
                if (m and float(m.group(1)) > 250) or any(h in line for h in HANG):
                    found.append(line[:240])
            if found:
                self.log(f"log watch: {found[0]}; quitting")
                self.ctl(["quit"], timeout=60)
                return
            stop.wait(1)

    def watch_builds(self, stop, seen):
        while not stop.is_set():
            b = ps(f"Get-Process {BUILD_PROCS} -EA SilentlyContinue | % {{ \"$($_.Id) $($_.Name)\" }}")
            if b and not seen:
                seen.append(b)
                self.log(f"a build is running: {b}; this run is contended")
            stop.wait(2)

    def measure(self, pid, result):
        self.ctl(["window", os.environ.get("WINDOW", "offscreen"), "--pid", str(pid)])
        if self.ctl(["run", "tests/game/boot.b3t"], timeout=900)[0]:
            return False
        if self.ctl(["run", "tests/game/perf_slice.b3t"], timeout=900)[0]:
            return False
        perf = subprocess.Popen([sys.executable, os.path.join(REPO, "tools", "perf_sample.py"),
                                 "--pid", str(pid), "--seconds", str(self.seconds), "--guard",
                                 "--port", PORT, "--out", os.path.join(self.out, "perf.jsonl")],
                                cwd=REPO, env=self.env, stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        self.ctl(["native_view on; present_stats reset; state"])
        perf.wait(timeout=self.seconds + 120)
        result["perf_exit"] = perf.returncode
        code, text = self.ctl(["native_view stats; present_stats; state"])
        replies = [json.loads(l) for l in text.strip().splitlines() if l.startswith("{")]
        if len(replies) == 3:
            result["native_view"], result["present"], result["state"] = replies
        return code == 0 and perf.returncode == 0

    def main(self):
        if not self.preconditions():
            return 4
        started = time.time()
        before = set(glob.glob(os.path.join(self.build, "logs", "band3_*.log")))
        code, text = self.ctl(["launch", "--exe", self.exe, "--user-data", self.ud, "--fresh",
                               "--timeout", "180", "--"] + self.flags)
        if code:
            return 1
        pid = json.loads(text.strip().splitlines()[-1])["pid"]
        new = sorted(set(glob.glob(os.path.join(self.build, "logs", "band3_*.log"))) - before)
        logname = new[-1] if new else None
        result = {"run": self.name, "flags": self.flags, "pid": pid, "log": logname, "exe": self.exe}
        stop, found, contended = threading.Event(), [], []
        threading.Thread(target=self.watch, args=(logname, stop, found), daemon=True).start()
        threading.Thread(target=self.watch_builds, args=(stop, contended), daemon=True).start()
        ok = False
        try:
            ok = self.measure(pid, result)
        finally:
            stop.set()
            self.ctl(["quit"], timeout=60)
            for _ in range(120):
                if str(pid) not in subprocess.run(["tasklist", "/FI", f"PID eq {pid}", "/NH"],
                                                  capture_output=True, text=True).stdout:
                    break
                time.sleep(1)
            hung = list(found)
            if logname and os.path.exists(logname):
                hung += [l.rstrip()[:240] for l in open(logname, encoding="utf-8", errors="replace")
                         if any(h in l for h in HANG)]
            ev = ps("Get-WinEvent -FilterHashtable @{LogName='System'; Id=4101; StartTime="
                    f"(Get-Date).AddSeconds(-{int(time.time() - started) + 30})}} "
                    "-EA SilentlyContinue | % { $_.TimeCreated.ToString('s') }")
            if ev:
                hung.append(f"event 4101 at {ev}")
            gpu = ps("Get-PnpDevice -Class Display -PresentOnly | % { \"$($_.Status) $($_.FriendlyName)\" }")
            if GPU_OK not in gpu:
                hung.append(f"adapter after: {gpu}")
            result["hang_signs"], result["contended"] = hung, contended
            with open(os.path.join(self.out, "result.json"), "w", encoding="utf-8") as f:
                json.dump(result, f, indent=1)
            self.log(f"hang signs: {hung}; contended: {bool(contended)}")
        if hung or result.get("perf_exit") == 2:
            return 3
        return 0 if ok else 1


def main(argv):
    if argv and argv[0] == "table":
        table(argv[1:])
        return 0
    exe = os.path.join(REPO, "out", "build", "win-amd64-release", "band3.exe")
    extra, seconds = [], 30
    while "--exe" in argv:
        i = argv.index("--exe")
        exe = os.path.abspath(argv[i + 1])
        del argv[i:i + 2]
    while "--flag" in argv:
        i = argv.index("--flag")
        extra.append(argv[i + 1])
        del argv[i:i + 2]
    while "--seconds" in argv:
        i = argv.index("--seconds")
        seconds = int(argv[i + 1])
        del argv[i:i + 2]
    if len(argv) != 2 or argv[0] not in ("60", "120"):
        print(__doc__)
        return 2
    return Run(argv[0], argv[1], exe, seconds, extra).main()


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
