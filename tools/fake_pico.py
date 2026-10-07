"""fake_pico: a stand-in for the RB3E Dashboard's Pico W wireless Stage Kit.

  python tools/fake_pico.py                 telemetry to 127.0.0.1:21071, prints what it's sent
  python tools/fake_pico.py --to 192.168.1.20 --name "Pico 1a:2b"

A real Pico (idio-sync/rb3e-stagekit-networked's firmware) answers a discovery
packet on UDP 21071 with telemetry, sent to whoever asked, and from then on keeps
sending it there; it takes RB3E's Stage Kit events on UDP 21070. This one sends
its telemetry unasked, once a second, as a Pico that already knows band3 does
(band3 under the test harness broadcasts no discovery), and keeps every RB3E Stage
Kit event that reaches 127.0.0.1:21070 (--listen) as (left, right): the game's,
through band3's RB3Enhanced events, and the Lights tab's test commands.
tools/test_stagekit_lights.py uses it.

Standard library only.
"""

import argparse
import json
import socket
import struct
import threading
import time

RB3E_PORT = 21070
TELEMETRY_PORT = 21071
STAGEKIT_EVENT = 6


class Event:
    def __init__(self, left, right):
        self.left = left
        self.right = right
        self.time = time.time()

    def __repr__(self):
        return f"[{self.left},{self.right}]"


class FakePico:
    """Sends telemetry to `to`:21071 and keeps the Stage Kit events sent to
    `listen`:21070 until stopped."""

    def __init__(self, to="127.0.0.1", listen="127.0.0.1", name="Pico fa:ke",
                 usb_status="Connected", wifi_signal=-48):
        self.to = to
        self.status = {"id": "28:cd:c1:00:fa:ce", "name": name, "usb_status": usb_status,
                       "wifi_signal": wifi_signal, "uptime": 0}
        self._events = []
        self._lock = threading.Lock()
        self._stop = threading.Event()
        self._commands = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._commands.bind((listen, RB3E_PORT))
        self._commands.settimeout(0.1)
        self._telemetry = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._threads = [threading.Thread(target=self._receive, daemon=True),
                         threading.Thread(target=self._send, daemon=True)]
        self._started = time.time()

    def start(self):
        for thread in self._threads:
            thread.start()
        return self

    def stop(self):
        self._stop.set()
        for thread in self._threads:
            thread.join(2)
        self._commands.close()
        self._telemetry.close()

    def events(self, since=0.0):
        with self._lock:
            return [e for e in self._events if e.time >= since]

    def wait_for(self, test, timeout):
        """`test()` once it's truthy, or None after `timeout` seconds."""
        deadline = time.time() + timeout
        while True:
            value = test()
            if value or time.time() >= deadline:
                return value or None
            time.sleep(0.05)

    def _send(self):
        while not self._stop.is_set():
            self.status["uptime"] = int(time.time() - self._started)
            try:
                self._telemetry.sendto(json.dumps(self.status).encode(), (self.to, TELEMETRY_PORT))
            except OSError:
                pass
            self._stop.wait(1.0)

    def _receive(self):
        while not self._stop.is_set():
            try:
                data, _ = self._commands.recvfrom(1024)
            except socket.timeout:
                continue
            except OSError:
                return
            # RB3E_EventHeader: 'RB3E', version, type, size, platform; then the data
            if len(data) < 10 or data[:4] != b"RB3E" or data[5] != STAGEKIT_EVENT:
                continue
            left, right = struct.unpack_from("BB", data, 8)
            with self._lock:
                self._events.append(Event(left, right))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--to", default="127.0.0.1", help="where band3 runs")
    parser.add_argument("--listen", default="127.0.0.1",
                        help="the address to take Stage Kit events on")
    parser.add_argument("--name", default="Pico fa:ke")
    args = parser.parse_args()
    pico = FakePico(args.to, args.listen, args.name).start()
    print(f"{args.name}: telemetry to {args.to}:{TELEMETRY_PORT}, "
          f"Stage Kit events on {args.listen}:{RB3E_PORT}; Ctrl+C stops", flush=True)
    seen = 0
    try:
        while True:
            events = pico.events()
            for event in events[seen:]:
                print(f"left 0x{event.left:02X} right 0x{event.right:02X}", flush=True)
            seen = len(events)
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        pico.stop()


if __name__ == "__main__":
    main()
