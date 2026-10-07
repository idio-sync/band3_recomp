"""settings_reference: writes docs/settings-reference.md from a build of band3.

  python tools/settings_reference.py              out/build/win-amd64-release's
  python tools/settings_reference.py --check      stop if the file isn't what the build writes
  python tools/settings_reference.py --build-dir out/build/linux-amd64-release

band3 --settings_reference=<file> writes the reference from its settings
registry and quits without starting the game (src/Launcher/settings_reference.h):
every setting the launcher and the in-game settings show, with its default, the values it takes
and its description. So it lists what that build registers, with the defaults
of the platform it was built for; the checked-in one is the Windows build's.
Change a setting's description in src/settings.cpp (or its label in
src/Launcher/launcher_settings.cpp), build, and run this. tools/package.py
checks it.

Standard library only.
"""

import argparse
import difflib
import os
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_BUILD = os.path.join("out", "build", "win-amd64-release")
REFERENCE = os.path.join(REPO, "docs", "settings-reference.md")


def generate(build_dir):
    """The reference the build in build_dir writes, with \\n line ends."""
    exe = os.path.join(build_dir, "band3.exe" if os.name == "nt" else "band3")
    if not os.path.isfile(exe):
        sys.exit(f"no {exe}: build it first")
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, "settings-reference.md")
        # from the temporary folder, so no band3_config.ini is found and its log
        # (logs/ beside the exe) is the only thing it leaves
        result = subprocess.run([exe, f"--settings_reference={out}"], cwd=tmp,
                                capture_output=True, text=True, timeout=120)
        if result.returncode != 0 or not os.path.isfile(out):
            sys.exit(f"{exe} --settings_reference failed ({result.returncode}): see its log")
        with open(out, encoding="utf-8") as f:
            return f.read()


def checked_in():
    """docs/settings-reference.md as committed (git may check it out with \\r\\n)."""
    if not os.path.isfile(REFERENCE):
        return ""
    with open(REFERENCE, encoding="utf-8") as f:
        return f.read()


def stale(text):
    """The first lines where the checked-in reference differs from `text`, or ''."""
    diff = list(difflib.unified_diff(checked_in().splitlines(), text.splitlines(),
                                     "docs/settings-reference.md", "the build's", lineterm="", n=0))
    return "\n".join(diff[:40])


def main(argv):
    p = argparse.ArgumentParser(description="Write docs/settings-reference.md from a build of band3.")
    p.add_argument("--build-dir", default=DEFAULT_BUILD,
                   help=f"the build folder, relative to the repository (default {DEFAULT_BUILD})")
    p.add_argument("--check", action="store_true",
                   help="don't write: exit 1 if the checked-in reference isn't the build's")
    args = p.parse_args(argv)

    text = generate(os.path.join(REPO, args.build_dir))
    if args.check:
        diff = stale(text)
        if diff:
            print(diff)
            print("docs/settings-reference.md isn't this build's: run python tools/settings_reference.py")
            return 1
        print("docs/settings-reference.md is up to date")
        return 0
    with open(REFERENCE, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)
    print(REFERENCE)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
