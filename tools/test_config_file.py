"""The launcher's band3.toml reaches the game: python tools/test_config_file.py

A harness test, so it needs the game built (and the unit tests, for the writer):
it writes a band3.toml with the launcher's own writer (band3_write_config, built
with the unit tests from src/Launcher/config_file.cpp), starts band3 with it
through `band3ctl launch --config`, and checks through the harness that the
game runs with its settings and folders. The harness keeps the launcher away,
so this is the file's way into the game, not the launcher's screen.

band3 runs from a temporary folder holding a band3_config.ini whose settings
all differ from the file's, so the ini's folder is the anchor for relative
paths, and every check fails if band3.toml isn't read or loses to the ini:

  game_data_root   absolute, with forward slashes; the ini's points nowhere,
                   so the game only boots if the file's is used
  cache_root       relative, so inside the temporary folder
  content_folders  one relative folder and one absolute
  controller_type  8 (the ini says 1)
  lang             eng (the ini says fre)
  user_data_root   in the file, but band3ctl sets it on the command line,
                   which wins

and, read back as written through the SDK's LoadConfig, one of each other kind
of value the writer emits:

  gold_on_all_difficulties  a bool, true
  song_speed                a float, 1.25
  midi_drums_device         a string with a quote and a backslash, escaped

The temporary folder (the cache with it) is deleted afterwards; band3ctl puts
back whatever band3.toml was beside the exe as soon as the game has read its own.

Exits 0 when every check passes, 1 otherwise.
"""

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time

import band3ctl

REPO = band3ctl.REPO
WRITER_NAME = "band3_write_config.exe" if os.name == "nt" else "band3_write_config"


def find_writer():
    """band3_write_config in out/tests, or in a configuration's folder there
    (Visual Studio's generator builds into Release/ or Debug/)."""
    for folder in ("", "Release", "Debug"):
        path = os.path.join(REPO, "out", "tests", folder, WRITER_NAME)
        if os.path.isfile(path):
            return path
    return os.path.join(REPO, "out", "tests", WRITER_NAME)

INI = """\
; band3_config.ini for tools/test_config_file.py: band3.toml should win over all of it
[controller]
type = 1

[game]
game_data_root = no_game_data_here
lang = fre
content_folders = ini_songs
cache_root = ini_cache
"""


# a string the writer has to escape: a quote and a backslash
QUOTED = 'Kit "A" \\ B'


def forward(path):
    """An absolute path with forward slashes, as the launcher writes one."""
    return os.path.abspath(path).replace("\\", "/")


def same_path(a, b):
    return os.path.normcase(os.path.normpath(a)) == os.path.normcase(os.path.normpath(b))


class Checks:
    def __init__(self):
        self.failed = []

    def check(self, what, ok, detail=""):
        print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail and not ok else ""))
        if not ok:
            self.failed.append(what)


def read_bytes(path):
    """The file's bytes, or None when it isn't there."""
    if not os.path.exists(path):
        return None
    with open(path, "rb") as f:
        return f.read()


def band3ctl_command(*words):
    return [sys.executable, os.path.join(REPO, "tools", "band3ctl.py"), *words]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", default=band3ctl.DEFAULT_EXE)
    parser.add_argument("--writer", default=find_writer(),
                        help="band3_write_config, built with the unit tests (out/tests)")
    parser.add_argument("--game-data", default=os.path.join(REPO, "assets"),
                        help="the game data folder to name in band3.toml")
    parser.add_argument("--port", type=int, default=band3ctl.DEFAULT_PORT)
    args = parser.parse_args(argv)

    if not os.path.isfile(args.writer):
        sys.exit(f"no band3_write_config at {args.writer}; build the unit tests (out/tests)")
    if not os.path.isdir(args.game_data):
        sys.exit(f"no game data at {args.game_data}")

    anchor = tempfile.mkdtemp(prefix="band3_config_test_")
    pid = None
    checks = Checks()
    try:
        with open(os.path.join(anchor, "band3_config.ini"), "w", encoding="utf-8") as f:
            f.write(INI)
        more_songs = os.path.join(anchor, "more songs")
        os.makedirs(os.path.join(anchor, "songs"))
        os.makedirs(more_songs)
        game_data = forward(args.game_data)
        settings = {
            "game_data_root": game_data,
            "cache_root": "cache",
            "user_data_root": forward(os.path.join(anchor, "not_this_user_data")),
            "content_folders": "songs|" + forward(more_songs),
            "controller_type": "8",
            "lang": "eng",
            "gold_on_all_difficulties": "true",
            "song_speed": "1.25",
            "midi_drums_device": QUOTED,
        }
        config = os.path.join(anchor, "band3.toml")
        subprocess.run([args.writer, config] + [f"{k}={v}" for k, v in settings.items()],
                       check=True)
        with open(config, encoding="utf-8") as f:
            print(f.read().rstrip())

        exe_toml = os.path.join(os.path.dirname(os.path.abspath(args.exe)), "band3.toml")
        before = read_bytes(exe_toml)
        launched = subprocess.run(
            band3ctl_command("--port", str(args.port), "launch", "--fresh", "--exe", args.exe,
                             "--cwd", anchor, "--config", config),
            capture_output=True, text=True)
        if launched.returncode != 0:
            print(launched.stdout + launched.stderr)
            checks.check("band3 starts with the file", False)
            return 1
        pid = json.loads(launched.stdout.strip().splitlines()[-1])["pid"]
        after = read_bytes(exe_toml)
        checks.check("band3ctl puts back what was beside the exe", before == after)

        conn = band3ctl.connect(args.port, wait_s=10)
        try:
            # the ini's game data folder doesn't exist: only the file's boots
            reply = conn.command("wait screen=splash_screen timeout=120s")
            checks.check("the game boots from the file's game data folder", reply.get("ok"),
                         json.dumps(reply))

            def cvar(name, value, source="config"):
                reply = conn.command(f"cvar {name}")
                got = reply.get("cvar", {})
                checks.check(f"{name} is {value} from {source}",
                             got.get("value") == value and got.get("source") == source,
                             json.dumps(reply))

            cvar("game_data_root", game_data)
            cvar("cache_root", "cache")
            cvar("content_folders", settings["content_folders"])
            cvar("controller_type", "8")
            cvar("lang", "eng")
            cvar("gold_on_all_difficulties", "true")
            cvar("midi_drums_device", QUOTED)
            reply = conn.command("cvar song_speed")
            got = reply.get("cvar", {})
            try:
                speed = float(got.get("value", ""))
            except ValueError:
                speed = None
            checks.check("song_speed is 1.25 from config",
                         speed == 1.25 and got.get("source") == "config", json.dumps(reply))
            cvar("user_data_root", os.path.abspath(os.path.join(REPO, "out", "test_user_data")),
                 "command_line")

            reply = conn.command("folders")
            folders = reply.get("folders", {})
            print(json.dumps(folders))
            checks.check("the game data folder is the file's",
                         same_path(folders.get("game_data", ""), args.game_data))
            checks.check("the cache is relative to the ini's folder",
                         same_path(folders.get("cache", ""), os.path.join(anchor, "cache")))
            checks.check("the user data folder is the command line's",
                         same_path(folders.get("user_data", ""),
                                   os.path.join(REPO, "out", "test_user_data")))
            content = folders.get("content", [])
            checks.check("the song folders are the file's, the relative one in the ini's folder",
                         len(content) == 2 and same_path(content[0], os.path.join(anchor, "songs"))
                         and same_path(content[1], more_songs))
            conn.command("quit")
        finally:
            conn.close()
    finally:
        if pid:
            wait_for_exit(pid)
        # the game may hold its cache open for a moment after it goes
        for _ in range(20):
            shutil.rmtree(anchor, ignore_errors=True)
            if not os.path.exists(anchor):
                break
            time.sleep(0.5)

    if checks.failed:
        print(f"FAIL {len(checks.failed)} of the checks")
        return 1
    print("PASS band3.toml reaches the game")
    return 0


def wait_for_exit(pid, timeout=30):
    """Waits for band3 to close, as `quit` asked it to."""
    if os.name != "nt":
        return
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32")
    kernel32.OpenProcess.restype = ctypes.c_void_p
    handle = kernel32.OpenProcess(0x00100000, False, pid)  # SYNCHRONIZE
    if not handle:
        return
    try:
        kernel32.WaitForSingleObject(ctypes.c_void_p(handle), timeout * 1000)
    finally:
        kernel32.CloseHandle(ctypes.c_void_p(handle))


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
