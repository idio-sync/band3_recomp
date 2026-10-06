"""symbolize: names the frames in a band3 crash report or log.

band3's crash reports (logs/crash-<start>-<pid>.txt beside band3.exe,
src/crash_trace.cpp) and its log give stack frames as module+offset and guest
code as guest addresses:

  #08 band3.exe+0x45FBA2E
  #03 guest 0x82517400 (band3.exe+0x1234)

This prints the text with each band3.exe frame named from the build's
band3.map, and each guest address inside a known function named from
band3_config.toml's [functions]:

  #08 band3.exe+0x45FBA2E [Band3App::OnPostSetup+0x1E]
  #03 guest 0x82517400 [CharClipSet__Load+0x0] (band3.exe+0x1234)

A report's header ("=== band3 <build> | band3.exe 6ac5002d | ...") gives
band3.exe's link time stamp, which the map that resolves it gives too
("Timestamp is 6ac5002d"): the map is picked by it from --map, the maps next to
the report (tools/package.py ships band3-<build>.map beside each zip) and
out/build/*/band3.map. Text before any header (a log, or a report from before
the header) uses --map or the newest out/build map, with a warning. Frames in
the SDK's DLLs (rexruntime.dll, rexgpu-xenos.dll) stay as they are: the SDK
ships no map for them.

  python tools/symbolize.py logs/crash-20261006-100533-1234.txt
  python tools/symbolize.py --map band3-v1.2-win-amd64-release.map report.txt
  python tools/symbolize.py out/build/win-amd64-release/logs/band3_012.log

C++ names stay mangled unless undname (Visual Studio's) is on the PATH.

Standard library only.
"""

import argparse
import bisect
import glob
import os
import re
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = re.compile(r"^=== band3 (?P<build>\S+) \| band3\.exe (?P<stamp>[0-9a-fA-F]{8}) \|")
EXE_FRAME = re.compile(r"\bband3\.exe\+0x(?P<rva>[0-9A-Fa-f]+)")
GUEST = re.compile(r"\bguest 0x(?P<addr>[0-9A-Fa-f]{8})")


class MapFile:
    """An MSVC linker map's symbols, by RVA."""

    def __init__(self, path, text=None):
        self.path = path
        if text is None:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        stamp = re.search(r"Timestamp is ([0-9a-fA-F]{8})", text)
        self.timestamp = stamp.group(1).lower() if stamp else None
        base = re.search(r"Preferred load address is ([0-9a-fA-F]+)", text)
        load = int(base.group(1), 16) if base else 0x140000000
        # "  0001:00177c80       __imp__App_dt     0000000140178c80 f   band3_recomp.0.cpp.obj":
        # Publics by Value, then Static symbols, in the same columns
        symbols = {}
        for m in re.finditer(r"^\s*[0-9a-fA-F]{4}:[0-9a-fA-F]{8}\s+(\S+)\s+([0-9a-fA-F]{16})\b",
                             text, re.M):
            va = int(m.group(2), 16)
            if va < load:
                continue
            symbols.setdefault(va - load, m.group(1))
        self.rvas = sorted(symbols)
        self.names = [symbols[r] for r in self.rvas]

    def lookup(self, rva):
        """(name, offset) of the symbol at or before rva, or None."""
        i = bisect.bisect_right(self.rvas, rva) - 1
        if i < 0:
            return None
        return self.names[i], rva - self.rvas[i]


class GuestFunctions:
    """band3_config.toml's [functions]: guest address -> name, size."""

    def __init__(self, path, text=None):
        if text is None:
            with open(path, encoding="utf-8", errors="replace") as f:
                text = f.read()
        entries = []
        for m in re.finditer(
                r'^0x([0-9A-Fa-f]{8})\s*=\s*\{\s*name\s*=\s*"([^"]*)"\s*,\s*size\s*=\s*0x([0-9A-Fa-f]+)',
                text, re.M):
            entries.append((int(m.group(1), 16), m.group(2), int(m.group(3), 16)))
        entries.sort()
        self.starts = [e[0] for e in entries]
        self.entries = entries

    def lookup(self, addr):
        """(name, offset) of the function addr is in, or None."""
        i = bisect.bisect_right(self.starts, addr) - 1
        if i < 0:
            return None
        start, name, size = self.entries[i]
        if addr >= start + size:
            return None
        return name, addr - start


class Demangler:
    """undname, when it's on the PATH, for C++ decorated names."""

    def __init__(self, enabled=True):
        self.tool = shutil.which("undname") if enabled else None
        self.cache = {}

    def __call__(self, name):
        if not name.startswith("?") or not self.tool:
            return name
        if name not in self.cache:
            try:
                out = subprocess.run([self.tool, name], capture_output=True, text=True,
                                     timeout=10).stdout
                m = re.search(r'is :- "(.*)"', out)
                self.cache[name] = m.group(1) if m else name
            except (OSError, subprocess.SubprocessError):
                self.cache[name] = name
        return self.cache[name]


def clean_name(name):
    # the recompiled functions are linked as __imp__<name>
    return name[len("__imp__"):] if name.startswith("__imp__") else name


def describe(hit, demangle=lambda n: n):
    name, offset = hit
    return "%s+0x%X" % (demangle(clean_name(name)), offset)


def candidate_maps(report_path, extra):
    paths = list(extra)
    if report_path:
        folder = os.path.dirname(os.path.abspath(report_path))
        paths += glob.glob(os.path.join(folder, "*.map"))
        paths += glob.glob(os.path.join(os.path.dirname(folder), "*.map"))
    paths += glob.glob(os.path.join(ROOT, "out", "build", "*", "band3.map"))
    paths += glob.glob(os.path.join(ROOT, "out", "package", "*.map"))
    seen, out = set(), []
    for p in paths:
        key = os.path.normcase(os.path.abspath(p))
        if key not in seen and os.path.isfile(p):
            seen.add(key)
            out.append(p)
    return out


def map_timestamp(path):
    """A map's "Timestamp is", from its first lines, without reading it all."""
    with open(path, encoding="utf-8", errors="replace") as f:
        for _ in range(10):
            m = re.search(r"Timestamp is ([0-9a-fA-F]{8})", f.readline())
            if m:
                return m.group(1).lower()
    return None


class Symbolizer:
    def __init__(self, maps, guest, demangle, warn=lambda s: print(s, file=sys.stderr)):
        # maps: candidate paths; the first is the fallback for text before a header
        self.candidates = maps
        self.guest = guest
        self.demangle = demangle
        self.warn = warn
        self.loaded = {}

    def _load(self, path):
        if path not in self.loaded:
            self.loaded[path] = MapFile(path)
        return self.loaded[path]

    def map_for(self, stamp):
        if stamp:
            for p in self.candidates:
                if map_timestamp(p) == stamp:
                    return self._load(p)
            self.warn("symbolize: no map with band3.exe's time stamp %s; band3.exe frames left "
                      "as they are (pass the build's map with --map)" % stamp)
            return None
        if not self.candidates:
            self.warn("symbolize: no band3.map found; band3.exe frames left as they are")
            return None
        path = self.candidates[0]
        self.warn("symbolize: no report header to match a map with; using %s, which may not be "
                  "the build that wrote this" % path)
        return self._load(path)

    def run(self, lines):
        current = None
        chosen = False
        for line in lines:
            header = HEADER.match(line)
            if header:
                current = self.map_for(header.group("stamp").lower())
                chosen = True
                yield line
                continue
            if not chosen and EXE_FRAME.search(line):
                current = self.map_for(None)
                chosen = True
            yield self.annotate(line, current)

    def annotate(self, line, map_file):
        named_guest = False

        def guest_sub(m):
            nonlocal named_guest
            hit = self.guest.lookup(int(m.group("addr"), 16)) if self.guest else None
            if not hit:
                return m.group(0)
            named_guest = True
            return "%s [%s]" % (m.group(0), describe(hit))

        line = GUEST.sub(guest_sub, line)
        if map_file and not named_guest:
            def exe_sub(m):
                hit = map_file.lookup(int(m.group("rva"), 16))
                return "%s [%s]" % (m.group(0), describe(hit, self.demangle)) if hit else m.group(0)
            line = EXE_FRAME.sub(exe_sub, line)
        return line


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("files", nargs="*", help="crash reports or logs (stdin when none)")
    ap.add_argument("--map", action="append", default=[],
                    help="a band3.map to use (repeatable); matched by time stamp")
    ap.add_argument("--config", default=os.path.join(ROOT, "band3_config.toml"),
                    help="band3_config.toml, for guest function names")
    ap.add_argument("--no-demangle", action="store_true", help="leave C++ names decorated")
    args = ap.parse_args(argv)

    guest = GuestFunctions(args.config) if os.path.isfile(args.config) else None
    if not guest:
        print("symbolize: no %s; guest addresses left as they are" % args.config, file=sys.stderr)
    demangle = Demangler(not args.no_demangle)

    sources = args.files or [None]
    for path in sources:
        maps = candidate_maps(path, args.map)
        # newest first, so the fallback is the latest build
        maps[len(args.map):] = sorted(maps[len(args.map):], key=os.path.getmtime, reverse=True)
        sym = Symbolizer(maps, guest, demangle)
        if path is None:
            lines = sys.stdin.read().splitlines()
        else:
            with open(path, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
        for out in sym.run(lines):
            print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
