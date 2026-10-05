"""port_mapping_check: band3 maps liveless_port on a mock router, each way it can.

Runs band3 (harness port 21181, a fresh profile, minimized and muted, through band3ctl)
against tools/port_mapping_mock.py on 127.0.0.1, once per scenario, and checks what
band3 reports (port_mapping_status), what it tells joining players (rooms_status's
advertised_ip and the log's "liveless: advertising" line, after going online from the
overshell as tests/game/liveless_online.b3t does) and that it deletes the mapping when
it quits:

  pcp      the router speaks PCP; band3 advertises the router's public address
  natpmp   PCP turned away, NAT-PMP works
  upnp     no answer over UDP (3.5 s), UPnP works
  upnp725  UPnP only maps for good: a permanent mapping (lease 0)
  upnp718  an old band3 mapping to this PC holds the port (ConflictInMappingEntry): band3
           deletes it and maps the port
  upnp718other  the mapping holding the port is another PC's: band3 leaves it alone
           and isn't mapped
  guard    no overrides: under the test harness band3 asks nothing (the mock, running,
           must log no request)
  setting  liveless_external_ip set: it wins over the mapping's address

Every launch but guard's passes both overrides, so nothing reaches the real router:
--liveless_gateway=127.0.0.1:5451 --liveless_upnp_url=http://127.0.0.1:8089/desc.xml.
guard's passes neither, and band3's harness guard keeps it off the router.

  python tools/port_mapping_check.py                  every scenario
  python tools/port_mapping_check.py pcp guard        some

Logs go to out/port_mapping_<scenario>_game.log and _mock.log. Exits 1 unless every
scenario passes. Standard library only.
"""

import argparse
import ctypes
import json
import os
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(REPO, "out")
CTL = [sys.executable, os.path.join(REPO, "tools", "band3ctl.py"), "--port", "21181"]
UDP_PORT, HTTP_PORT = 5451, 8089
OVERRIDES = [f"--liveless_gateway=127.0.0.1:{UDP_PORT}",
             f"--liveless_upnp_url=http://127.0.0.1:{HTTP_PORT}/desc.xml"]
PUBLIC = "203.0.113.5"
SETTING = "198.51.100.9"

# name: (mock arguments, game arguments, port_mapping_status checks (the first the state
# to wait for), advertised address and where it's from or None to stay offline, the
# delete the mock must log, then optionally what the mock must log in that order, a
# "!" before what it mustn't log at all)
SCENARIOS = {
    "pcp": (["--mode", "pcp"], OVERRIDES,
            ["state=mapped", "method=pcp", f"external_ip={PUBLIC}", "port=9103", "lease_s=3600"],
            (PUBLIC, "port mapping"), "pcp: delete UDP 9103"),
    "natpmp": (["--mode", "natpmp"], OVERRIDES,
               ["state=mapped", "method=natpmp", f"external_ip={PUBLIC}", "port=9103",
                "lease_s=3600"],
               None, "natpmp: delete UDP 9103"),
    "upnp": (["--mode", "silent"], OVERRIDES,
             ["state=mapped", "method=upnp", f"external_ip={PUBLIC}", "port=9103", "lease_s=3600"],
             None, "upnp: delete UDP 9103"),
    "upnp725": (["--mode", "silent", "--upnp-error", "725"], OVERRIDES,
                ["state=mapped", "method=upnp", f"external_ip={PUBLIC}", "port=9103", "lease_s=0"],
                None, "upnp: delete UDP 9103"),
    "upnp718": (["--mode", "silent", "--upnp-error", "718"], OVERRIDES,
                ["state=mapped", "method=upnp", f"external_ip={PUBLIC}", "port=9103",
                 "lease_s=3600"],
                None, "upnp: delete UDP 9103",
                ["upnp: map UDP 9103 lifetime 3600 for 127.0.0.1:9103 -> error 718",
                 "upnp: GetSpecificPortMappingEntry UDP 9103 -> 127.0.0.1",
                 "upnp: delete UDP 9103 (the old mapping)",
                 "upnp: map UDP 9103 lifetime 3600 for 127.0.0.1:9103\n",
                 # at quit
                 "upnp: delete UDP 9103\n"]),
    "upnp718other": (["--mode", "silent", "--upnp-error", "718", "--stale-client", "192.0.2.7"],
                     OVERRIDES,
                     # a check is one word
                     ["state=failed", "method=upnp", "error~192.0.2.7"],
                     None, None,
                     ["upnp: map UDP 9103 lifetime 3600 for 127.0.0.1:9103 -> error 718",
                      "upnp: GetSpecificPortMappingEntry UDP 9103 -> 192.0.2.7",
                      "!upnp: delete"]),
    "guard": (["--mode", "pcp"], [],
              ["state=off", "error~harness"],
              None, None),
    "setting": (["--mode", "pcp"], OVERRIDES + [f"--liveless_external_ip={SETTING}"],
                ["state=mapped", "method=pcp", f"external_ip={PUBLIC}"],
                (SETTING, "setting"), "pcp: delete UDP 9103"),
}

# tests/game/liveless_online.b3t's way online from the overshell, with each press held
# 600 ms and a pause on each screen, so a game drawing a few frames a second (a busy
# GPU) still sees them
ONLINE = ["wait screen=splash_screen timeout=120s", "press start 600",
          "wait screen=first_time_calibration timeout=30s", "sleep 2s", "press down 600",
          "press green 600", "wait screen=dx_welcome_screen timeout=30s", "sleep 2s",
          "press green 600", "wait screen=hint_rb3_welcome_screen timeout=30s", "sleep 2s",
          "press green 600", "wait screen=manage_band_screen timeout=30s", "sleep 2s",
          "press red 600", "wait screen=main_hub_screen timeout=30s", "sleep 2s",
          # the overshell: Play on Xbox Live, then back out of it
          "press start 600", "sleep 3s", "press down 600", "sleep 2s", "press green 600",
          "sleep 6s", "press up 600", "sleep 2s", "press green 600", "sleep 3s"]

# what the mock logs for a request; the guard's log must have none
REQUEST_MARKS = ("pcp:", "natpmp:", "upnp:", "udp:", "http:")


def ctl(*args, capture=False):
    result = subprocess.run(CTL + list(args), cwd=REPO, capture_output=capture, text=True)
    return result


def check(*args):
    result = ctl(*args, capture=True)
    line = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr
    print(f"  {' '.join(args)}\n    -> {line[:400]}")
    if result.returncode:
        raise AssertionError(f"failed: {' '.join(args)}")
    return line


def wait_exit(pid, timeout_s=15):
    """Waits for the game's process to end, without signalling it."""
    if os.name != "nt":
        time.sleep(timeout_s / 3)
        return
    synchronize = 0x00100000
    handle = ctypes.windll.kernel32.OpenProcess(synchronize, False, pid)
    if not handle:
        return  # gone already
    try:
        if ctypes.windll.kernel32.WaitForSingleObject(handle, int(timeout_s * 1000)) != 0:
            raise AssertionError(f"band3 (pid {pid}) still running {timeout_s} s after quit")
    finally:
        ctypes.windll.kernel32.CloseHandle(handle)


def start_mock(log_path, args):
    log = open(log_path, "w", encoding="utf-8")
    mock = subprocess.Popen(
        [sys.executable, os.path.join(REPO, "tools", "port_mapping_mock.py"),
         "--udp-port", str(UDP_PORT), "--http-port", str(HTTP_PORT), *args],
        cwd=REPO, stdout=log, stderr=subprocess.STDOUT)
    log.close()
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        if mock.poll() is not None:
            raise AssertionError(f"the mock exited; see {log_path}")
        with open(log_path, encoding="utf-8", errors="replace") as f:
            if "listening on" in f.read():
                return mock
        time.sleep(0.2)
    mock.terminate()
    raise AssertionError(f"the mock didn't start; see {log_path}")


def read(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.read()


def run(name):
    mock_args, game_args, checks, advertised, delete, *rest = SCENARIOS[name]
    sequence = rest[0] if rest else []
    game_log = os.path.join(OUT, f"port_mapping_{name}_game.log")
    mock_log = os.path.join(OUT, f"port_mapping_{name}_mock.log")
    if os.path.exists(game_log):
        os.remove(game_log)
    print(f"{name}: mock {' '.join(mock_args)}; game {' '.join(game_args) or '(no overrides)'}")
    mock = start_mock(mock_log, mock_args)
    pid = None
    try:
        launched = check("launch", "--fresh", "--", "--liveless=true", "--username=host",
                         *game_args, f"--log_file={game_log}")
        pid = json.loads(launched)["pid"]
        if name == "guard":
            # Start ran before the harness answered; give a UDP leg time to show
            check("sleep 5s")
        else:
            check(f"wait port_mapping={checks[0].partition('=')[2]} timeout=30s")
        check("port_mapping_status", *checks)
        if advertised:
            for command in ONLINE:
                check(command)
            # the address the game told players joining it (Rooms or not)
            check("rooms_status", f"advertised_ip={advertised[0]}")
    finally:
        if pid:
            ctl("quit", capture=True)
            wait_exit(pid)
        time.sleep(0.5)
        mock.terminate()
        mock.wait()
    mock_text, game_text = read(mock_log), read(game_log)
    # each line is "<time> <what>"
    requests = [l for l in mock_text.splitlines()
                if l.partition(" ")[2].startswith(REQUEST_MARKS)]
    print("  mock: " + ("\n        ".join(requests) if requests else "(no requests)"))
    print("  game: " + "\n        ".join(l.split("] ", 4)[-1] for l in game_text.splitlines()
                                        if "port mapping" in l or "liveless: advertising" in l))
    if delete and delete not in mock_text:
        raise AssertionError(f"the mock has no '{delete}' after quit")
    at = 0
    for text in sequence:
        if text.startswith("!"):
            if text[1:] in mock_text:
                raise AssertionError(f"the mock has '{text[1:]}'")
            continue
        found = mock_text.find(text, at)
        if found < 0:
            raise AssertionError(f"the mock has no '{text.strip()}' after what came before")
        at = found + len(text)
    if name == "guard":
        if requests:
            raise AssertionError("band3 sent the mock something under the guard")
        if "port mapping: skipped under the test harness" not in game_text:
            raise AssertionError("no 'skipped under the test harness' in the game's log")
    if advertised:
        line = f"liveless: advertising {advertised[0]} ({advertised[1]})"
        if line not in game_text:
            raise AssertionError(f"no '{line}' in the game's log")


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("scenarios", nargs="*",
                        help="which to run (all by default): " + ", ".join(SCENARIOS))
    args = parser.parse_args()
    unknown = [name for name in args.scenarios if name not in SCENARIOS]
    if unknown:
        parser.error("no scenario " + ", ".join(unknown))
    os.makedirs(OUT, exist_ok=True)
    failed = []
    for name in args.scenarios or list(SCENARIOS):
        try:
            run(name)
            print(f"{name}: PASS")
        except AssertionError as e:
            print(f"{name}: FAIL: {e}")
            failed.append(name)
    print("FAIL: " + ", ".join(failed) if failed else "PASS: every scenario")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
