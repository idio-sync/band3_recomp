"""perf_sample: what a running band3 costs the machine, sampled from outside it.

Once a second (--interval) for --seconds, from Windows' own counters (PDH) and
the process's times, without the harness:

  cpu_pct     the process's CPU, as a share of every logical CPU (Task
              Manager's), and cpu_cores, the same in cores
  gpu3d_pct   GPU Engine's Utilization Percentage summed over the process's 3D
              engines (instances pid_<pid>_..._engtype_3D: each GPU queue the
              process uses on each adapter)
  vram_mb     GPU Process Memory's Dedicated Usage over the process's instances

each sample a JSON line (to --out, else stdout), then a summary line: each
one's mean and peak, and every thread's CPU time over the run (kernel and
user, in ms, from its times at the start and the end; a thread that started
or ended meanwhile counts from or to that), by thread id, with its name
where it has one (SetThreadDescription) and, with --log, the first line the
band3 log has from it: band3's log tags each line [t<id>] with the thread's
id, so the game thread, the native renderer's worker and the sync-only
GPU's threads can be told apart.

  python tools/perf_sample.py --pid 1234 --seconds 20 --out out/n7/perf/off.jsonl
  python tools/perf_sample.py --pid 1234 --seconds 60 --guard --port 21095
  python tools/perf_sample.py --pid 1234 --seconds 20 --log out/build/win-amd64-release/logs/band3_012.log

--guard stops a run that has the GPU flat out, for the 120 Hz throughput runs
(out/research/n7_3_design.md): once the 3D engines are over --guard-pct (95)
for --guard-seconds (5) straight, it prints ABORT, quits the game through the
harness on --port if given (band3ctl's `quit`), writes the summary and exits 2.
Without --port it only reports.

Windows only; standard library only (ctypes).
"""

import argparse
import ctypes
import json
import os
import sys
import time
from ctypes import wintypes

PDH_FMT_DOUBLE = 0x00000200
PDH_FMT_LARGE = 0x00000400
PDH_MORE_DATA = 0x800007D2
PDH_CSTATUS_VALID_DATA = 0x0
PDH_CSTATUS_NEW_DATA = 0x1
PROCESS_QUERY_LIMITED_INFORMATION = 0x1000
THREAD_QUERY_LIMITED_INFORMATION = 0x0800
TH32CS_SNAPTHREAD = 0x00000004

GPU_3D = r"\GPU Engine(*)\Utilization Percentage"
GPU_MEMORY = r"\GPU Process Memory(*)\Dedicated Usage"


class FmtValue(ctypes.Structure):
    class Value(ctypes.Union):
        _fields_ = [("long", ctypes.c_long), ("double", ctypes.c_double),
                    ("large", ctypes.c_longlong)]
    _fields_ = [("status", wintypes.DWORD), ("value", Value)]


class FmtItem(ctypes.Structure):
    _fields_ = [("name", wintypes.LPWSTR), ("value", FmtValue)]


class ThreadEntry(ctypes.Structure):
    _fields_ = [("size", wintypes.DWORD), ("usage", wintypes.DWORD),
                ("thread_id", wintypes.DWORD), ("owner_pid", wintypes.DWORD),
                ("base_priority", ctypes.c_long), ("delta_priority", ctypes.c_long),
                ("flags", wintypes.DWORD)]


def instance_matches(name, pid, engine=None):
    """Whether a GPU counter instance (pid_<pid>_luid_..._phys_0[_eng_N_engtype_<type>])
    is the process's, and of that engine type if given."""
    if not name.startswith(f"pid_{pid}_"):
        return False
    return engine is None or name.endswith(f"engtype_{engine}")


def summarize(samples, threads_start, threads_end, names=None, log_lines=None):
    """The summary: each measure's mean and peak over the samples, and each
    thread's CPU ms over the run, most first."""
    out = {"samples": len(samples)}
    for key in ("cpu_pct", "cpu_cores", "gpu3d_pct", "vram_mb"):
        values = [s[key] for s in samples if s.get(key) is not None]
        out[key] = ({"mean": round(sum(values) / len(values), 3), "peak": round(max(values), 3)}
                    if values else None)
    threads = []
    for tid in set(threads_start) | set(threads_end):
        ms = threads_end.get(tid, threads_start.get(tid, 0)) - threads_start.get(tid, 0)
        entry = {"tid": tid, "cpu_ms": round(ms, 1)}
        if tid not in threads_start:
            entry["started"] = True
        if tid not in threads_end:
            entry["ended"] = True
        if names and names.get(tid):
            entry["name"] = names[tid]
        if log_lines and tid in log_lines:
            entry["log"] = log_lines[tid]
        threads.append(entry)
    threads.sort(key=lambda t: -t["cpu_ms"])
    out["threads"] = threads
    return out


def read_log_tags(path):
    """thread id -> the first line the log has from it (after the tag)."""
    first = {}
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            at = line.find("[t")
            end = line.find("]", at)
            if at < 0 or end < 0:
                continue
            tag = line[at + 2:end]
            if tag.isdigit() and int(tag) not in first:
                first[int(tag)] = line[end + 1:].strip()[:120]
    return first


class Guard:
    """Over limit for `seconds` straight: the moment it trips."""

    def __init__(self, limit, seconds):
        self.limit, self.seconds, self.since = limit, seconds, None

    def check(self, t, value):
        if value is None or value <= self.limit:
            self.since = None
            return False
        if self.since is None:
            self.since = t
        return t - self.since >= self.seconds


class Sampler:
    """The process's counters, Windows only."""

    def __init__(self, pid):
        self.pid = pid
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self.k32 = k32
        k32.OpenProcess.restype = wintypes.HANDLE
        k32.OpenThread.restype = wintypes.HANDLE
        k32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
        self.process = k32.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, False, pid)
        if not self.process:
            raise OSError(f"can't open process {pid} (error {ctypes.get_last_error()})")
        self.cpus = os.cpu_count() or 1
        self.pdh = ctypes.WinDLL("pdh")
        self.query = wintypes.HANDLE()
        self._check(self.pdh.PdhOpenQueryW(None, 0, ctypes.byref(self.query)), "PdhOpenQuery")
        self.gpu = self._counter(GPU_3D)
        self.memory = self._counter(GPU_MEMORY)
        # a rate needs two collections
        self.pdh.PdhCollectQueryData(self.query)
        self.last = (time.perf_counter(), self.process_ms())

    def _check(self, status, what):
        if status != 0:
            raise OSError(f"{what} failed: 0x{status & 0xFFFFFFFF:08X}")

    def _counter(self, path):
        counter = wintypes.HANDLE()
        status = self.pdh.PdhAddEnglishCounterW(self.query, path, 0, ctypes.byref(counter))
        if status != 0:
            # no GPU counters (no WDDM 2 driver): the measure is left out
            print(f"perf_sample: no counter {path} (0x{status & 0xFFFFFFFF:08X})",
                  file=sys.stderr)
            return None
        return counter

    def _values(self, counter, fmt):
        """name -> value for every instance of a wildcard counter."""
        if counter is None:
            return None
        size, count = wintypes.DWORD(0), wintypes.DWORD(0)
        status = self.pdh.PdhGetFormattedCounterArrayW(counter, fmt, ctypes.byref(size),
                                                       ctypes.byref(count), None)
        if status & 0xFFFFFFFF != PDH_MORE_DATA:
            return {}
        buf = ctypes.create_string_buffer(size.value)
        status = self.pdh.PdhGetFormattedCounterArrayW(counter, fmt, ctypes.byref(size),
                                                       ctypes.byref(count), buf)
        if status != 0:
            return {}
        items = ctypes.cast(buf, ctypes.POINTER(FmtItem))
        out = {}
        for i in range(count.value):
            item = items[i]
            if item.value.status not in (PDH_CSTATUS_VALID_DATA, PDH_CSTATUS_NEW_DATA):
                continue
            v = item.value.value.double if fmt == PDH_FMT_DOUBLE else item.value.value.large
            out[item.name] = out.get(item.name, 0) + v
        return out

    @staticmethod
    def _ms(kernel, user):
        ticks = lambda f: (f.dwHighDateTime << 32) | f.dwLowDateTime
        return (ticks(kernel) + ticks(user)) / 1e4

    def process_ms(self):
        c, e, k, u = (wintypes.FILETIME() for _ in range(4))
        if not self.k32.GetProcessTimes(self.process, ctypes.byref(c), ctypes.byref(e),
                                        ctypes.byref(k), ctypes.byref(u)):
            raise OSError(f"GetProcessTimes failed: {ctypes.get_last_error()}")
        return self._ms(k, u)

    def alive(self):
        code = wintypes.DWORD()
        return bool(self.k32.GetExitCodeProcess(self.process, ctypes.byref(code))) and \
            code.value == 259  # STILL_ACTIVE

    def sample(self):
        self.pdh.PdhCollectQueryData(self.query)
        now, ms = time.perf_counter(), self.process_ms()
        wall = (now - self.last[0]) * 1000
        cores = (ms - self.last[1]) / wall if wall > 0 else 0
        self.last = (now, ms)
        s = {"cpu_cores": round(cores, 3), "cpu_pct": round(100 * cores / self.cpus, 2)}
        gpu = self._values(self.gpu, PDH_FMT_DOUBLE)
        s["gpu3d_pct"] = None if gpu is None else round(
            sum(v for n, v in gpu.items() if instance_matches(n, self.pid, "3D")), 2)
        memory = self._values(self.memory, PDH_FMT_LARGE)
        s["vram_mb"] = None if memory is None else round(
            sum(v for n, v in memory.items() if instance_matches(n, self.pid)) / 2**20, 1)
        return s

    def threads(self):
        """thread id -> (CPU ms, name) for the process's threads now."""
        out = {}
        snap = self.k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
        if not snap or snap == wintypes.HANDLE(-1).value:
            return out
        try:
            entry = ThreadEntry()
            entry.size = ctypes.sizeof(ThreadEntry)
            more = self.k32.Thread32First(snap, ctypes.byref(entry))
            while more:
                if entry.owner_pid == self.pid:
                    out[entry.thread_id] = self._thread(entry.thread_id)
                more = self.k32.Thread32Next(snap, ctypes.byref(entry))
        finally:
            self.k32.CloseHandle(snap)
        return {tid: v for tid, v in out.items() if v is not None}

    def _thread(self, tid):
        h = self.k32.OpenThread(THREAD_QUERY_LIMITED_INFORMATION, False, tid)
        if not h:
            return None
        try:
            c, e, k, u = (wintypes.FILETIME() for _ in range(4))
            if not self.k32.GetThreadTimes(h, ctypes.byref(c), ctypes.byref(e), ctypes.byref(k),
                                           ctypes.byref(u)):
                return None
            name = None
            describe = getattr(self.k32, "GetThreadDescription", None)
            if describe:
                text = wintypes.LPWSTR()
                if describe(h, ctypes.byref(text)) >= 0 and text.value:
                    name = text.value
                if text:
                    self.k32.LocalFree(text)
            return self._ms(k, u), name
        finally:
            self.k32.CloseHandle(h)

    def close(self):
        self.pdh.PdhCloseQuery(self.query)
        self.k32.CloseHandle(self.process)


def quit_game(port):
    """band3ctl's `quit` on the harness's port; whether it answered."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import band3ctl
    try:
        conn = band3ctl.connect(port)
        try:
            return bool(conn.command("quit").get("ok"))
        finally:
            conn.close()
    except OSError as e:
        print(f"perf_sample: couldn't quit the game on port {port}: {e}", file=sys.stderr)
        return False


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--pid", type=int, required=True, help="the process to sample")
    ap.add_argument("--seconds", type=float, required=True, help="how long")
    ap.add_argument("--interval", type=float, default=1.0, help="seconds between samples")
    ap.add_argument("--out", help="JSON lines here (the samples, then the summary)")
    ap.add_argument("--log", help="band3's log, to name each thread by its first line there")
    ap.add_argument("--guard", action="store_true",
                    help="abort when the GPU's 3D engines are over --guard-pct for "
                         "--guard-seconds straight (exit 2)")
    ap.add_argument("--guard-pct", type=float, default=95.0)
    ap.add_argument("--guard-seconds", type=float, default=5.0)
    ap.add_argument("--port", type=int, help="with --guard, quit the game on this harness port")
    a = ap.parse_args()
    if os.name != "nt":
        sys.exit("perf_sample: Windows only")

    sampler = Sampler(a.pid)
    out = open(a.out, "w", encoding="utf-8") if a.out else sys.stdout
    guard = Guard(a.guard_pct, a.guard_seconds) if a.guard else None
    samples, aborted, ended = [], None, False
    try:
        start_threads = sampler.threads()
        started = time.perf_counter()
        next_at = started + a.interval
        while True:
            time.sleep(max(0.0, next_at - time.perf_counter()))
            next_at += a.interval
            if not sampler.alive():
                ended = True
                break
            s = sampler.sample()
            s["t"] = round(time.perf_counter() - started, 3)
            samples.append(s)
            out.write(json.dumps(s) + "\n")
            out.flush()
            if guard and guard.check(s["t"], s["gpu3d_pct"]):
                aborted = (f"GPU 3D over {a.guard_pct:g}% for {a.guard_seconds:g} s "
                           f"(now {s['gpu3d_pct']}%)")
                print(f"ABORT: {aborted}", file=sys.stderr)
                if a.port:
                    print("ABORT: quitting the game: "
                          + ("ok" if quit_game(a.port) else "no answer"), file=sys.stderr)
                break
            if s["t"] >= a.seconds:
                break
        end_threads = {} if ended else sampler.threads()
    finally:
        sampler.close()
    names = {tid: v[1] for d in (start_threads, end_threads) for tid, v in d.items() if v[1]}
    summary = summarize(samples, {t: v[0] for t, v in start_threads.items()},
                        {t: v[0] for t, v in end_threads.items()}, names,
                        read_log_tags(a.log) if a.log else None)
    summary.update(pid=a.pid, seconds=round(samples[-1]["t"], 3) if samples else 0,
                   aborted=aborted, process_ended=ended)
    out.write(json.dumps({"summary": summary}) + "\n")
    if out is not sys.stdout:
        out.close()
        brief = {k: summary[k] for k in ("samples", "cpu_pct", "gpu3d_pct", "vram_mb")}
        print(json.dumps(brief))
        for t in summary["threads"][:8]:
            print(f"  t{t['tid']}: {t['cpu_ms']} ms  {t.get('name') or ''}  {t.get('log', '')}")
    return 2 if aborted else 0


if __name__ == "__main__":
    sys.exit(main())
