"""LAUNCHES THE GAME: band3's Home Assistant link against a fake broker, through a song.

python tools/test_home_assistant.py   (a harness test: it needs the game built, and
starts band3 minimized and muted through band3ctl, so ask before running it on a
machine someone is using)

It starts tools/ha_fake_broker.py's MQTT broker and webhook receiver on free
127.0.0.1 ports, then band3 on a fresh test profile (out/test_user_data_ha) with

  --ha_mqtt_host=127.0.0.1 --ha_mqtt_port=<broker>
  --ha_webhook_url=http://127.0.0.1:<receiver>/api/webhook/rb3_event --ha_stagekit=true

and checks, in order:

  - band3 connects, with band3/<id>/status = offline (retained) as its will; <id> is
    read from that will topic, so the PC's name doesn't matter
  - every discovery config (the nine game entities and the six Stage Kit ones) is
    retained under homeassistant/<component>/band3_<id>/<entity>/config with its
    unique_id, state and availability topics and device; status is online (retained)
  - through tests/game/boot.b3t and the menus (as tests/game/render_song.b3t goes),
    20th Century Boy on autoplay: playing ON, song non-empty, progress rising, a
    score above 0, and the webhook's {"type":"song",...} then {"type":"state",
    "status":"playing"}
  - the pause menu: paused ON; its Back to Music Library: playing OFF, paused OFF,
    and the webhook's {"type":"state","status":"menu"}
  - band3 killed (not quit, so no DISCONNECT): the broker publishes the will and
    band3/<id>/status is retained as offline

band3's log goes to out/<name>.log and every broker and webhook event to
out/<name>_broker.log (--name, test_home_assistant by default). Exits 0 when every
check passes, 1 otherwise; band3 is killed and the broker stopped either way.

Standard library only.
"""

import argparse
import datetime
import json
import os
import subprocess
import sys
import time

import band3ctl
import ha_fake_broker

REPO = band3ctl.REPO
OUT = os.path.join(REPO, "out")

GAME_ENTITIES = {"song": "sensor", "artist": "sensor", "venue": "sensor", "score": "sensor",
                 "playing": "binary_sensor", "paused": "binary_sensor", "screen": "sensor",
                 "progress": "sensor", "band": "sensor"}
STAGEKIT_ENTITIES = {"stagekit_red": "sensor", "stagekit_yellow": "sensor",
                     "stagekit_green": "sensor", "stagekit_blue": "sensor",
                     "stagekit_strobe": "sensor", "stagekit_fog": "binary_sensor"}

# from where boot.b3t leaves a fresh profile (the welcome hint) to 20th Century Boy
# on autoplay, as tests/game/render_song.b3t and pacing_song.b3t go
TO_SONG = """
wait screen=hint_rb3_welcome_screen timeout=30s
sleep 1s
press green until screen=manage_band_screen timeout=30s
sleep 1s
press red until screen=main_hub_screen timeout=30s
sleep 1s
set autoplay true
# Play Now, Quickplay, Choose Songs
press green
sleep 1500ms
press green
sleep 1500ms
press green until screen=song_select_screen timeout=30s
sleep 1s
# past the 123 heading to 20th Century Boy
press down
press down
sleep 500ms
press green until screen=part_difficulty_screen timeout=30s
sleep 1500ms
# Guitar, Expert
press green
sleep 500ms
press green
wait in_game timeout=60s
expect song=20thcenturyboy
"""

PAUSE = """
press start
sleep 1s
"""

# the pause menu's Back to Music Library, the fifth of six, and yes to losing the
# progress, as tests/game/render_screens_song.b3t leaves its first song
LEAVE_SONG = """
press down
press down
press down
press down
sleep 333ms
press green
sleep 500ms
press green until screen=song_select_screen timeout=30s
wait menus timeout=30s
"""


class Failed(Exception):
    """A check the rest can't go on without."""


class Checks:
    def __init__(self):
        self.failed = []

    def check(self, what, ok, detail=""):
        print(("ok   " if ok else "FAIL ") + what + (f": {detail}" if detail and not ok else ""),
              flush=True)
        if not ok:
            self.failed.append(what)
        return ok

    def require(self, what, ok, detail=""):
        if not self.check(what, ok, detail):
            raise Failed(what)


def script(conn, text, name, checks):
    """Runs harness commands (a .b3t's text); a failing one fails the test."""
    commands = band3ctl.parse_script(text)
    result = band3ctl.run_script(conn, commands, name)
    checks.require(f"{name} runs", result.passed,
                   f"line {result.line}: {result.command}: {json.dumps(result.reply)}")


def game_state(conn):
    return conn.command("state").get("state", {})


def is_int(text, minimum=None):
    try:
        value = int(text)
    except (TypeError, ValueError):
        return False
    return minimum is None or value >= minimum


def check_discovery(broker, checks, id_):
    expected = {**GAME_ENTITIES, **STAGEKIT_ENTITIES}
    topics = {entity: f"homeassistant/{component}/band3_{id_}/{entity}/config"
              for entity, component in expected.items()}
    broker.wait_for(lambda: all(broker.retained(t) for t in topics.values()), 20)
    retained = broker.retained()
    for entity, topic in topics.items():
        text = retained.get(topic)
        if not checks.check(f"discovery config {topic} is retained", text is not None,
                            f"retained discovery topics: "
                            f"{sorted(t for t in retained if t.startswith('homeassistant/'))}"):
            continue
        try:
            config = json.loads(text)
        except ValueError:
            checks.check(f"{entity}'s config is JSON", False, text)
            continue
        device = config.get("device", {})
        checks.check(f"{entity}'s config names its topics and device",
                     config.get("unique_id") == f"band3_{id_}_{entity}"
                     and config.get("state_topic") == f"band3/{id_}/{entity}"
                     and config.get("availability_topic") == f"band3/{id_}/status"
                     and device.get("identifiers") == [f"band3_{id_}"]
                     and device.get("manufacturer") == "band3", text)


def progress_values(broker, id_, since):
    return [int(p.text) for p in broker.publishes(f"band3/{id_}/progress")
            if p.time >= since and is_int(p.text)]


def webhook_payloads(broker):
    return [h.json for h in broker.webhooks()]


def summary(broker, id_):
    """The last payload of each band3/<id>/ topic, for a failure's reader."""
    last = {}
    for p in broker.publishes():
        if id_ and p.topic.startswith(f"band3/{id_}/"):
            last[p.topic] = p.text
    return last


def kill(pid):
    """Ends band3 at once (TerminateProcess), as a crash would: no DISCONNECT."""
    if os.name == "nt":
        subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
    else:
        import signal
        try:
            os.kill(pid, signal.SIGKILL)
        except OSError:
            pass


def wait_for_exit(pid, timeout=30):
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


def ha_log_lines(log_path, limit=40):
    """The band3 log's `ha:` lines, the last `limit` of them."""
    try:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            lines = [line.rstrip() for line in f if "ha:" in line]
    except OSError:
        return [f"(no log at {log_path})"]
    return lines[-limit:]


def play(args, broker, checks, log_path):
    """Everything from the launch to the kill; returns the PC's <id>. band3's pid is
    kept in args.pid while it runs, for main to kill on a failure."""
    webhook_url = f"{broker.webhook_url_base}/api/webhook/rb3_event"
    launch = band3ctl_command(
        "--port", str(args.port), "launch", "--fresh", "--exe", args.exe,
        "--user-data", args.user_data, "--",
        "--ha_mqtt_host=127.0.0.1", f"--ha_mqtt_port={broker.mqtt_port}",
        f"--ha_webhook_url={webhook_url}", "--ha_stagekit=true", f"--log_file={log_path}")
    print(" ".join(launch), flush=True)
    launched = subprocess.run(launch, capture_output=True, text=True, cwd=REPO)
    pid = None
    if launched.returncode == 0:
        try:
            pid = json.loads(launched.stdout.strip().splitlines()[-1])["pid"]
        except (ValueError, KeyError, IndexError):
            pass
    args.pid = pid
    checks.require("band3 starts under the harness", pid is not None,
                   (launched.stdout + launched.stderr).strip())

    # the connection and its will
    connected = broker.wait_for(lambda: [c for c in broker.connects() if c.return_code == 0],
                                60)
    checks.require("band3 connects to the broker within 60 s", connected,
                   f"connects: {broker.connects()}")
    connect = connected[0]
    will = connect.will_topic or ""
    parts = will.split("/")
    checks.require("its will is band3/<id>/status", len(parts) == 3 and parts[0] == "band3"
                   and parts[2] == "status" and parts[1], f"will topic {will!r}")
    id_ = parts[1]
    print(f"     id {id_}, client {connect.client_id!r}, keepalive {connect.keepalive_s} s",
          flush=True)
    checks.check("the will is offline, retained",
                 connect.will_payload == b"offline" and connect.will_retain,
                 f"{connect.will_payload!r}, retain {connect.will_retain}")
    checks.check("it connects with a client id and a clean session",
                 bool(connect.client_id) and connect.clean_session)
    checks.check("it connects without a user name (none was set)", connect.username is None)

    status = f"band3/{id_}/status"
    checks.check(f"{status} is retained as online",
                 broker.wait_for(lambda: broker.retained(status) == "online", 20),
                 f"retained {broker.retained(status)!r}")
    check_discovery(broker, checks, id_)
    topic = lambda entity: f"band3/{id_}/{entity}"
    for entity in [*GAME_ENTITIES, *STAGEKIT_ENTITIES]:
        checks.check(f"{topic(entity)} is published",
                     broker.wait_for(lambda: broker.latest(topic(entity)) is not None, 5))
    checks.check("playing is OFF in the menus", broker.latest(topic("playing")) == "OFF",
                 broker.latest(topic("playing")))

    conn = band3ctl.connect(args.port, wait_s=10)
    try:
        with open(os.path.join(REPO, "tests", "game", "boot.b3t"), encoding="utf-8") as f:
            script(conn, f.read(), "boot", checks)
        webhooks_before = len(broker.webhooks())
        script(conn, TO_SONG, "to the song", checks)
        in_song = time.time()

        checks.check("playing turns ON",
                     broker.wait_for(lambda: broker.latest(topic("playing")) == "ON", 10),
                     broker.latest(topic("playing")))
        state = game_state(conn)
        title = state.get("song", {}).get("name", "")
        checks.check("song is the song's title",
                     broker.wait_for(lambda: broker.latest(topic("song")), 10)
                     and (not title or broker.latest(topic("song")) == title),
                     f"{broker.latest(topic('song'))!r}, the game's {title!r}")
        checks.check("artist is set", bool(broker.latest(topic("artist"))))
        attributes = broker.latest(topic("song/attributes"))
        try:
            shortname = json.loads(attributes or "{}").get("shortname")
        except ValueError:
            shortname = None
        checks.check("song's attributes have the shortname", shortname == "20thcenturyboy",
                     attributes)

        def started():
            hooks = webhook_payloads(broker)[webhooks_before:]
            types = [(h or {}).get("type") for h in hooks]
            return ("song" in types and {"type": "state", "status": "playing"} in hooks
                    and hooks)

        hooks = broker.wait_for(started, 10) or webhook_payloads(broker)[webhooks_before:]
        song_at = next((i for i, h in enumerate(hooks)
                        if (h or {}).get("type") == "song"), None)
        playing_at = next((i for i, h in enumerate(hooks)
                           if h == {"type": "state", "status": "playing"}), None)
        checks.check('the webhook gets {"type":"song"} then {"type":"state","status":"playing"}',
                     song_at is not None and playing_at is not None and song_at < playing_at,
                     json.dumps(hooks))
        if song_at is not None:
            checks.check("the song webhook names the song",
                         bool(hooks[song_at].get("name"))
                         and (not title or hooks[song_at].get("name") == title),
                         json.dumps(hooks[song_at]))
        posts = broker.webhooks()[webhooks_before:]
        checks.check("each webhook POST is JSON to /api/webhook/rb3_event",
                     posts and all(h.path == "/api/webhook/rb3_event"
                                   and h.content_type.startswith("application/json")
                                   and h.json is not None for h in posts),
                     str([(h.path, h.content_type, h.body) for h in posts]))

        # autoplay scores
        script(conn, "wait score>=1 timeout=60s", "the score", checks)
        checks.check("score goes above 0",
                     broker.wait_for(lambda: is_int(broker.latest(topic("score")), 1), 10),
                     broker.latest(topic("score")))

        def rising():
            values = progress_values(broker, id_, in_song)
            return len(values) >= 2 and values[-1] > values[0] and values
        # 20th Century Boy is about 3.5 minutes: 1% every 2 s or so
        values = broker.wait_for(rising, 45) or progress_values(broker, id_, in_song)
        checks.check("progress rises", bool(values) and values[-1] > values[0], str(values))
        times = [p.time for p in broker.publishes(topic("progress")) if p.time >= in_song]
        gaps = [b - a for a, b in zip(times, times[1:])]
        checks.check("progress publishes at most about once a second",
                     all(gap >= 0.9 for gap in gaps), str([round(g, 2) for g in gaps]))
        changed = [e for e in STAGEKIT_ENTITIES
                   if len([p for p in broker.publishes(topic(e)) if p.time >= in_song]) > 0]
        print(f"     Stage Kit topics that changed in the song: {changed or 'none'}", flush=True)

        # the pause menu
        script(conn, PAUSE, "pause", checks)
        checks.check("paused turns ON in the pause menu",
                     broker.wait_for(lambda: broker.latest(topic("paused")) == "ON", 10),
                     broker.latest(topic("paused")))
        webhooks_before = len(broker.webhooks())
        script(conn, LEAVE_SONG, "back to the music library", checks)
        checks.check("playing turns OFF",
                     broker.wait_for(lambda: broker.latest(topic("playing")) == "OFF", 10),
                     broker.latest(topic("playing")))
        checks.check("paused turns OFF",
                     broker.wait_for(lambda: broker.latest(topic("paused")) == "OFF", 10),
                     broker.latest(topic("paused")))
        hooks = broker.wait_for(
            lambda: {"type": "state", "status": "menu"} in webhook_payloads(broker)[webhooks_before:]
            and webhook_payloads(broker)[webhooks_before:], 10) \
            or webhook_payloads(broker)[webhooks_before:]
        checks.check('the webhook gets {"type":"state","status":"menu"}',
                     {"type": "state", "status": "menu"} in hooks, json.dumps(hooks))
        checks.check("song keeps its last value after the song",
                     bool(broker.latest(topic("song"))), broker.latest(topic("song")))
    finally:
        conn.close()

    # a crash, as far as the broker can tell
    publishes_before = len(broker.publishes())
    kill(pid)
    wait_for_exit(pid)
    args.pid = None
    checks.check("the broker publishes the will when band3 is killed",
                 broker.wait_for(lambda: any(p.will for p in broker.publishes()[publishes_before:]),
                                 15),
                 f"disconnects: {broker.disconnects()}")
    checks.check(f"{status} is retained as offline", broker.retained(status) == "offline",
                 f"retained {broker.retained(status)!r}")
    gone = broker.disconnects()
    checks.check("band3 didn't send DISCONNECT (it was killed)",
                 bool(gone) and not gone[-1].clean, str(gone))
    return id_


def band3ctl_command(*words):
    return [sys.executable, os.path.join(REPO, "tools", "band3ctl.py"), *words]


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--exe", default=band3ctl.DEFAULT_EXE)
    parser.add_argument("--port", type=int, default=band3ctl.DEFAULT_PORT,
                        help="the harness port")
    parser.add_argument("--user-data", default=os.path.join(OUT, "test_user_data_ha"),
                        help="the test profile, emptied first")
    parser.add_argument("--name", default="test_home_assistant",
                        help="the logs' names, under out/")
    args = parser.parse_args(argv)

    if not os.path.isfile(args.exe):
        sys.exit(f"no band3 at {args.exe}")
    try:
        band3ctl.Connection(args.port).close()
        sys.exit(f"a band3 already answers on harness port {args.port}; quit it or pick --port")
    except OSError:
        pass
    os.makedirs(OUT, exist_ok=True)
    log_path = os.path.join(OUT, f"{args.name}.log")
    broker_log = open(os.path.join(OUT, f"{args.name}_broker.log"), "w", encoding="utf-8")

    def log(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        broker_log.write(f"{stamp} {line}\n")
        broker_log.flush()

    broker = ha_fake_broker.FakeBroker(log=log).start()
    print(f"fake broker on 127.0.0.1:{broker.mqtt_port}, webhook on {broker.webhook_url_base}",
          flush=True)
    checks = Checks()
    args.pid = None
    id_ = None
    try:
        id_ = play(args, broker, checks, log_path)
    except Failed:
        pass
    except OSError as e:
        checks.check("the harness answers", False, str(e))
    finally:
        if args.pid:
            kill(args.pid)
            wait_for_exit(args.pid)
        broker.stop()
        broker_log.close()

    if checks.failed:
        print(f"FAIL {len(checks.failed)} of the checks")
        connects = broker.connects()
        if not id_ and connects and connects[0].will_topic:
            parts = connects[0].will_topic.split("/")
            id_ = parts[1] if len(parts) > 1 else None
        print("band3/<id>/ topics' last payloads:")
        for topic, payload in sorted(summary(broker, id_).items()):
            print(f"  {topic} = {payload[:120]!r}")
        print(f"band3's ha: log lines ({log_path}):")
        for line in ha_log_lines(log_path):
            print(f"  {line}")
        print(f"every broker event: {os.path.join(OUT, args.name + '_broker.log')}")
        return 1
    print("PASS band3 tells the fake Home Assistant broker and webhook about a song")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
