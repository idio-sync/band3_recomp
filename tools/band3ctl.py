"""band3ctl: drives band3 through its test harness.

band3 started with --test_port=<port> takes one command per line on
127.0.0.1:<port> and answers each with one line of JSON. This sends them:

  python tools/band3ctl.py launch                 start band3 (minimized) and wait for it
  python tools/band3ctl.py state                  any harness command, e.g.
  python tools/band3ctl.py press green+strum_down
  python tools/band3ctl.py wait screen~main timeout=90s
  python tools/band3ctl.py run tests/game/boot.b3t

`run` replays a .b3t script: harness commands one per line, # comments. It
stops at the first command that fails, saves a screenshot of the moment, and
exits 1. The commands are listed in the README's Test harness section.

Standard library only.
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass, field

DEFAULT_PORT = 21071
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_EXE = os.path.join(REPO, "out", "build", "win-amd64-release", "band3.exe")


def parse_script(text):
    """The commands in a .b3t script, as (line number, command) pairs."""
    commands = []
    for number, line in enumerate(text.splitlines(), start=1):
        # a comment starts a line or follows whitespace, so band#1 stays whole
        line = re.sub(r"(^|\s)#.*$", "", line).strip()
        if line:
            commands.append((number, line))
    return commands


def parse_reply(data):
    """One reply line as a dict; a line that isn't JSON reads as a failure."""
    text = data.decode("utf-8", errors="replace").strip()
    try:
        return json.loads(text)
    except ValueError:
        return {"ok": False, "error": "not a harness reply: " + text}


@dataclass
class ScriptResult:
    passed: bool
    line: int = 0
    command: str = ""
    reply: dict = field(default_factory=dict)


def run_script(conn, commands, name):
    """Sends each command in turn; stops at the first failure and screenshots it."""
    for number, command in commands:
        reply = conn.command(command)
        if not reply.get("ok"):
            safe = re.sub(r"[^A-Za-z0-9_-]", "_", name)
            conn.command(f"screenshot fail-{safe}-line{number}")
            return ScriptResult(False, number, command, reply)
    return ScriptResult(True)


class Connection:
    def __init__(self, port, timeout=None):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        # a wait can take as long as its timeout says
        self.sock.settimeout(timeout)
        self.buffer = b""

    def command(self, line):
        self.sock.sendall(line.encode("utf-8") + b"\n")
        while b"\n" not in self.buffer:
            chunk = self.sock.recv(65536)
            if not chunk:
                return {"ok": False, "error": "band3 closed the connection"}
            self.buffer += chunk
        reply, self.buffer = self.buffer.split(b"\n", 1)
        return parse_reply(reply)

    def close(self):
        self.sock.close()


def connect(port, wait_s=0.0):
    """Connects, retrying for up to wait_s seconds while band3 starts."""
    deadline = time.monotonic() + wait_s
    while True:
        try:
            return Connection(port)
        except OSError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.5)


def launch(args):
    exe = os.path.abspath(args.exe)
    if not os.path.isfile(exe):
        sys.exit(f"no band3 at {exe}")
    # band3 finds band3_config.ini and its game data (assets/) from here
    cwd = os.path.abspath(args.cwd)
    # its own saves and profile, so a test never touches the player's
    user_data = os.path.abspath(args.user_data)
    if args.fresh and os.path.isdir(user_data):
        shutil.rmtree(user_data)
    os.makedirs(user_data, exist_ok=True)
    command = [exe, f"--test_port={args.port}", f"--user_data_root={user_data}"] + args.extra
    flags = 0
    startupinfo = None
    if os.name == "nt":
        flags = subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP
        if not args.show:
            # minimized and never activated, so it doesn't take the keyboard
            # from whatever is in front; the game keeps running and drawing
            startupinfo = subprocess.STARTUPINFO()
            startupinfo.dwFlags |= subprocess.STARTF_USESHOWWINDOW
            startupinfo.wShowWindow = 7  # SW_SHOWMINNOACTIVE
    process = subprocess.Popen(command, cwd=cwd, creationflags=flags, startupinfo=startupinfo,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    deadline = time.monotonic() + args.timeout
    while time.monotonic() < deadline:
        if process.poll() is not None:
            sys.exit(f"band3 exited with code {process.returncode} before the harness answered")
        try:
            conn = Connection(args.port)
            reply = conn.command("state")
            conn.close()
            print(json.dumps({"ok": True, "pid": process.pid, "state": reply.get("state")}))
            return 0
        except OSError:
            time.sleep(0.5)
    sys.exit(f"band3 (pid {process.pid}) didn't answer on port {args.port} "
             f"within {args.timeout} s; is test_port in use?")


def run(args):
    with open(args.script, encoding="utf-8") as f:
        commands = parse_script(f.read())
    name = os.path.splitext(os.path.basename(args.script))[0]
    conn = connect(args.port)
    try:
        started = time.monotonic()
        result = run_script(conn, commands, name)
    finally:
        conn.close()
    if result.passed:
        print(f"PASS {args.script}: {len(commands)} commands in "
              f"{time.monotonic() - started:.1f} s")
        return 0
    print(f"FAIL {args.script}:{result.line}: {result.command}")
    print(json.dumps(result.reply))
    return 1


def main(argv):
    parser = argparse.ArgumentParser(
        description="Drive band3 through its test harness.",
        epilog="Anything else is sent as a harness command, e.g. `state` or `press green`.")
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = parser.add_subparsers(dest="action")

    p = sub.add_parser("launch", help="start band3 with the harness and wait for it")
    p.add_argument("--exe", default=DEFAULT_EXE)
    p.add_argument("--cwd", default=REPO,
                   help="where band3 runs from (finds band3_config.ini and assets/ there)")
    p.add_argument("--timeout", type=float, default=120)
    p.add_argument("--user-data", default=os.path.join(REPO, "out", "test_user_data"),
                   help="saves and profile for the test run, kept apart from the player's")
    p.add_argument("--fresh", action="store_true",
                   help="start from an empty --user-data, as a first boot (for scripts)")
    p.add_argument("--show", action="store_true",
                   help="open the window normally (by default it starts minimized, "
                        "without taking focus)")
    p.add_argument("extra", nargs="*", help="more band3 arguments, e.g. --fast_start=true")

    p = sub.add_parser("run", help="replay a .b3t script")
    p.add_argument("script")

    known = {"launch", "run", "-h", "--help"}
    rest = argv[:]
    port_args = []
    while rest and rest[0].startswith("--port"):
        port_args += rest[:2] if rest[0] == "--port" else rest[:1]
        rest = rest[2:] if rest[0] == "--port" else rest[1:]
    if rest and rest[0] not in known:
        port = parser.parse_args(port_args).port
        conn = connect(port)
        try:
            reply = conn.command(" ".join(rest))
        finally:
            conn.close()
        print(json.dumps(reply))
        return 0 if reply.get("ok") else 1

    args = parser.parse_args(argv)
    if args.action == "launch":
        return launch(args)
    if args.action == "run":
        return run(args)
    parser.print_help()
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
