"""package: zips a build of band3 for someone else to play.

  python tools/package.py                     out/build/win-amd64-release
  python tools/package.py --build-dir out/build/linux-amd64-release
  python tools/package.py --allow-dirty       a build with uncommitted changes

Build first: this packages what the build folder holds. It writes
out/package/band3-<commit>-<build folder>.zip, everything under a band3 folder:

  band3.exe (band3 on Linux) and the libraries beside it (rexruntime, rexgpu-xenos)
  settings-reference.md: every setting, its default and what it does, as
    docs/settings-reference.md has it (tools/settings_reference.py)
  LICENSE.md, and licenses/ for the libraries built into band3: each
    src/ThirdParty folder's LICENSE.txt (RtMidi's notice, from its header), and the
    SDK's licenses folder (SDL3)
  README.txt: what to install, where the game files go, and where the source is

No game files: the player brings their own. band3.map goes beside the zip
(band3-<commit>-<build folder>.map), not in it: it resolves the frames a crash
trace from that build logs, so keep it for as long as the build is out there.
band3.pdb goes beside it the same way (band3-<commit>-<build folder>.pdb): its
public symbols name the frames in that build's minidumps, in WinDbg.

The commit in the name is HEAD's, and README.txt links its source, as GPLv2
asks of a binary. With uncommitted changes to tracked files the build may not
be that commit, so it stops unless --allow-dirty, which adds -dirty to the name.
A Windows build also writes its settings reference, and it stops if that isn't
docs/settings-reference.md (also unless --allow-dirty), since the zip's would
then be wrong for it.

Standard library only.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import zipfile

import settings_reference

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join("out", "build", "win-amd64-release")
OUT = os.path.join(REPO, "out", "package")
FALLBACK_URL = "https://github.com/idio-sync/band3_recomp"
VC_REDIST_URL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"


def git(*args):
    return subprocess.run(["git", *args], cwd=REPO, check=True, capture_output=True,
                          text=True).stdout.strip()


def source_url():
    """The repository's web address, from origin (ssh or https)."""
    try:
        url = git("remote", "get-url", "origin")
    except subprocess.CalledProcessError:
        return FALLBACK_URL
    m = re.match(r"git@([^:]+):(.+)", url)
    if m:
        url = f"https://{m.group(1)}/{m.group(2)}"
    return url.removesuffix(".git") if url.startswith("https://") else FALLBACK_URL


def binaries(build_dir):
    """band3's executable and the shared libraries beside it, and whether it's Windows's."""
    names = sorted(os.listdir(build_dir))
    windows = "band3.exe" in names
    exe = "band3.exe" if windows else "band3"
    if exe not in names:
        sys.exit(f"no band3 in {build_dir}: build it first")
    libs = [n for n in names
            if n.endswith(".dll") or n.endswith(".so") or ".so." in n]
    if not libs:
        sys.exit(f"no rexruntime library beside band3 in {build_dir}")
    return [exe, *libs], windows


def rtmidi_license():
    """RtMidi's notice, from the top of its header: it has no license file."""
    with open(os.path.join(REPO, "src", "ThirdParty", "rtmidi", "RtMidi.h"),
              encoding="utf-8") as f:
        text = f.read()
    start = text.index("RtMidi: realtime MIDI i/o C++ classes")
    end = text.index("*/", start)
    return "\n".join(line.strip() for line in text[start:end].splitlines()).strip() + "\n"


def third_party_licenses():
    """{name in licenses/: its text} for every library built into band3: one
    file per library, or a folder for an SDK library with several."""
    def read(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()

    out = {}
    third_party = os.path.join(REPO, "src", "ThirdParty")
    for lib in sorted(os.listdir(third_party)):
        path = os.path.join(third_party, lib, "LICENSE.txt")
        if os.path.isfile(path):
            out[f"{lib}.txt"] = read(path)
    out["rtmidi.txt"] = rtmidi_license()
    sdk = os.path.join(REPO, ".rexglue-sdk", "licenses")
    if not os.path.isdir(sdk):
        print(f"warning: no {sdk}, so the zip lacks the SDK's libraries' licenses",
              file=sys.stderr)
        return out
    for lib in sorted(os.listdir(sdk)):
        folder = os.path.join(sdk, lib)
        names = sorted(n for n in os.listdir(folder)
                       if os.path.isfile(os.path.join(folder, n)))
        for n in names:
            out[f"{lib}.txt" if len(names) == 1 else f"{lib}/{n}"] = read(os.path.join(folder, n))
    return out


def readme(commit, full_commit, windows, dirty):
    lines = [
        f"band3 {commit}{' (with uncommitted changes)' if dirty else ''}: "
        "Rock Band 3, recompiled for PC.",
        "",
        "Before the first start:",
    ]
    if windows:
        lines += [
            "- Install the Microsoft Visual C++ Redistributable (x64), a recent one:",
            f"  {VC_REDIST_URL}",
        ]
    lines += [
        "- Bring your own Rock Band 3 (Xbox 360): the Title Update 5 default.xex, or",
        "  Rock Band 3 Deluxe's, and the gen folder with the main and patch ARK files.",
        "  Put them in a folder named assets beside band3, or pick their folder in",
        "  the launcher's Game tab, which opens on the first start. No game files are",
        "  included.",
        "",
        "Settings: the launcher, which "
        + ("holding Shift as band3 starts brings back" if windows
           else "starting band3 with --launcher brings back")
        + ", and F4 in game.",
        "settings-reference.md lists every setting, its default and what it does.",
        "",
        "If band3 crashes, the next start says so. The logs folder beside band3 has",
        "the log and the crash report (crash-*.txt"
        + (", and a crash-*.dmp beside it" if windows else "")
        + "): include them when you report the problem.",
        "",
        f"Source: {source_url()}/tree/{full_commit} (GPLv2, see LICENSE.md).",
        "licenses/ holds the licenses of the libraries band3 is built with.",
        "",
    ]
    return "\n".join(lines)


def main(argv):
    p = argparse.ArgumentParser(description="Zip a build of band3 for someone else to play.")
    p.add_argument("--build-dir", default=DEFAULT_BUILD,
                   help=f"the build folder, relative to the repository (default {DEFAULT_BUILD})")
    p.add_argument("--allow-dirty", action="store_true",
                   help="package although tracked files have uncommitted changes")
    args = p.parse_args(argv)

    build_dir = os.path.join(REPO, args.build_dir)
    if not os.path.isdir(build_dir):
        sys.exit(f"no build folder {build_dir}")
    files, windows = binaries(build_dir)

    dirty = bool(git("status", "--porcelain", "--untracked-files=no"))
    if dirty and not args.allow_dirty:
        sys.exit("tracked files have uncommitted changes, so the build may not be HEAD's: "
                 "commit them, or pass --allow-dirty")
    # the checked-in reference is the Windows build's (its defaults are Windows')
    if windows:
        diff = settings_reference.stale(settings_reference.generate(build_dir))
        if diff and not args.allow_dirty:
            sys.exit(f"{diff}\ndocs/settings-reference.md isn't this build's: run "
                     "python tools/settings_reference.py and commit it, or pass --allow-dirty")
    commit = git("rev-parse", "--short", "HEAD")
    full_commit = git("rev-parse", "HEAD")
    name = f"band3-{commit}{'-dirty' if dirty else ''}-{os.path.basename(os.path.normpath(build_dir))}"

    os.makedirs(OUT, exist_ok=True)
    zip_path = os.path.join(OUT, name + ".zip")
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in files:
            info = zipfile.ZipInfo.from_file(os.path.join(build_dir, f), f"band3/{f}")
            info.compress_type = zipfile.ZIP_DEFLATED
            if f == "band3":
                info.external_attr = 0o755 << 16
            with open(os.path.join(build_dir, f), "rb") as src:
                z.writestr(info, src.read())
        z.write(settings_reference.REFERENCE, "band3/settings-reference.md")
        z.write(os.path.join(REPO, "LICENSE.md"), "band3/LICENSE.md")
        for license_name, text in third_party_licenses().items():
            z.writestr(f"band3/licenses/{license_name}", text)
        z.writestr("band3/README.txt", readme(commit, full_commit, windows, dirty))
        packed = z.namelist()

    print(zip_path)
    for n in packed:
        print(f"  {n}")
    map_file = os.path.join(build_dir, "band3.map")
    if os.path.isfile(map_file):
        shutil.copy2(map_file, os.path.join(OUT, name + ".map"))
        print(os.path.join(OUT, name + ".map"), "(keep: resolves this build's crash traces)")
    pdb_file = os.path.join(build_dir, "band3.pdb")
    if os.path.isfile(pdb_file):
        shutil.copy2(pdb_file, os.path.join(OUT, name + ".pdb"))
        print(os.path.join(OUT, name + ".pdb"), "(keep: names the frames in this build's minidumps)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
