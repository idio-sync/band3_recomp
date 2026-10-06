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

A Linux report's frames are band3+0x1234 and its header names the executable
by its GNU build id ("band3 build-id 0a1b..."): they're named with addr2line
(binutils) from the executable with that build id, from --elf, the report's
folder's parent (where the executable is) and out/build/linux-*/band3.

  python tools/symbolize.py logs/crash-20261006-100533-1234.txt
  python tools/symbolize.py --map band3-v1.2-win-amd64-release.map report.txt
  python tools/symbolize.py out/build/win-amd64-release/logs/band3_012.log
  python3 tools/symbolize.py --elf ~/band3/band3 ~/band3/logs/crash-20261006-100533-1234.txt

C++ names stay mangled unless undname (Visual Studio's) is on the PATH.

Standard library only.
"""

import argparse
import bisect
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

HEADER = re.compile(r"^=== band3 (?P<build>\S+) \| (?:band3\.exe (?P<stamp>[0-9a-fA-F]{8})"
                    r"|band3 build-id (?P<build_id>[0-9a-f]+|unknown)) \|")
EXE_FRAME = re.compile(r"\bband3\.exe\+0x(?P<rva>[0-9A-Fa-f]+)")
ELF_FRAME = re.compile(r"(?<![\w.])band3\+0x(?P<offset>[0-9A-Fa-f]+)")
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


def elf_build_id(data):
    """A 64-bit little-endian ELF's GNU build id, in hex, from its notes; or None."""
    if data[:4] != b"\x7fELF" or data[4] != 2 or data[5] != 1:
        return None
    phoff, = struct.unpack_from("<Q", data, 0x20)
    phentsize, phnum = struct.unpack_from("<HH", data, 0x36)
    for i in range(phnum):
        p_type, _, p_offset, _, _, p_filesz = struct.unpack_from("<IIQQQQ", data,
                                                                 phoff + i * phentsize)
        if p_type != 4:  # PT_NOTE
            continue
        pos, end = p_offset, min(p_offset + p_filesz, len(data))
        while pos + 12 <= end:
            namesz, descsz, n_type = struct.unpack_from("<III", data, pos)
            name_at = pos + 12
            desc_at = name_at + ((namesz + 3) & ~3)
            if n_type == 3 and data[name_at:name_at + namesz] == b"GNU\x00":  # NT_GNU_BUILD_ID
                return data[desc_at:desc_at + descsz].hex()
            pos = desc_at + ((descsz + 3) & ~3)
    return None


def file_build_id(path):
    # the notes are in the first pages
    with open(path, "rb") as f:
        return elf_build_id(f.read(1 << 16))


class ElfFile:
    """A Linux executable's functions, by offset, through addr2line."""

    def __init__(self, path, tool=None):
        self.path = path
        self.tool = tool or shutil.which("addr2line")
        self.cache = {}

    def lookup_many(self, offsets):
        todo = [o for o in offsets if o not in self.cache]
        if todo and self.tool:
            try:
                out = subprocess.run([self.tool, "-C", "-f", "-e", self.path] +
                                     ["0x%x" % o for o in todo],
                                     capture_output=True, text=True, timeout=60).stdout.splitlines()
            except (OSError, subprocess.SubprocessError):
                out = []
            # two lines each: the function, then file:line
            for i, o in enumerate(todo):
                name = out[2 * i] if 2 * i < len(out) else "??"
                self.cache[o] = None if name == "??" else name
        return {o: self.cache.get(o) for o in offsets}


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


def candidate_elfs(report_path, extra):
    paths = list(extra)
    if report_path:
        folder = os.path.dirname(os.path.abspath(report_path))
        paths += [os.path.join(folder, "band3"), os.path.join(os.path.dirname(folder), "band3")]
    paths += sorted(glob.glob(os.path.join(ROOT, "out", "build", "linux-*", "band3")),
                    key=os.path.getmtime, reverse=True)
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
    def __init__(self, maps, guest, demangle, warn=lambda s: print(s, file=sys.stderr),
                 elfs=(), open_elf=ElfFile):
        # maps, elfs: candidate paths; the first is the fallback for text before a header
        self.candidates = maps
        self.elf_candidates = list(elfs)
        self.open_elf = open_elf
        self.guest = guest
        self.demangle = demangle
        self.warn = warn
        self.loaded = {}

    def elf_for(self, build_id):
        if build_id and build_id != "unknown":
            for p in self.elf_candidates:
                if file_build_id(p) == build_id:
                    return self.open_elf(p)
            self.warn("symbolize: no executable with build id %s; band3 frames left as they are "
                      "(pass the build's executable with --elf)" % build_id)
            return None
        if not self.elf_candidates:
            self.warn("symbolize: no Linux band3 executable found; band3 frames left as they are")
            return None
        path = self.elf_candidates[0]
        self.warn("symbolize: no build id to match an executable with; using %s, which may not "
                  "be the build that wrote this" % path)
        return self.open_elf(path)

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
        current = elf = None
        chosen = elf_chosen = False
        # a Linux section's lines with frames, whose offsets go to addr2line at once
        pending = []
        out = []
        for line in lines:
            header = HEADER.match(line)
            if header:
                self._name_elf_frames(elf, pending, out)
                if header.group("stamp"):
                    current = self.map_for(header.group("stamp").lower())
                    chosen = True
                else:
                    elf = self.elf_for(header.group("build_id"))
                    elf_chosen = True
                out.append(line)
                continue
            if not chosen and EXE_FRAME.search(line):
                current = self.map_for(None)
                chosen = True
            if not elf_chosen and ELF_FRAME.search(line):
                elf = self.elf_for(None)
                elf_chosen = True
            line = self.annotate(line, current)
            if elf and ELF_FRAME.search(line):
                pending.append(len(out))
            out.append(line)
        self._name_elf_frames(elf, pending, out)
        return out

    def _name_elf_frames(self, elf, pending, out):
        if elf and pending:
            offsets = {int(m.group("offset"), 16)
                       for i in pending for m in ELF_FRAME.finditer(out[i])}
            names = elf.lookup_many(sorted(offsets))

            def sub(m):
                name = names.get(int(m.group("offset"), 16))
                return "%s [%s]" % (m.group(0), name) if name else m.group(0)
            for i in pending:
                out[i] = ELF_FRAME.sub(sub, out[i])
        pending.clear()

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
    ap.add_argument("--elf", action="append", default=[],
                    help="a Linux band3 executable to use (repeatable); matched by build id")
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
        sym = Symbolizer(maps, guest, demangle, elfs=candidate_elfs(path, args.elf))
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
