"""ha_fake_broker: a stand-in MQTT broker and webhook receiver for band3's Home Assistant link.

band3 tells Home Assistant about the game two ways (docs/integrations.md, Home
Assistant): MQTT discovery through a broker, and the RB3E dashboard's webhook POSTs.
This is both, on 127.0.0.1 only, so the link is tested without a broker or Home
Assistant:

  python tools/ha_fake_broker.py --mqtt-port 1883 --webhook-port 8124

and band3 started with

  --ha_mqtt_host=127.0.0.1 --ha_mqtt_port=1883
  --ha_webhook_url=http://127.0.0.1:8124/api/webhook/rb3_event

The broker speaks just enough MQTT 3.1.1 for one publisher: it reads CONNECT (client
id, user name and password, the will), answers CONNACK (--connack-code, or 4 when
--username/--password are given and the client's don't match), PINGRESP to PINGREQ,
PUBACK to a QoS 1 PUBLISH and SUBACK to SUBSCRIBE, and records every PUBLISH with a
store of the retained ones (an empty retained payload deletes its topic). It forwards
nothing to subscribers. A client that closes without DISCONNECT, or stays silent past
one and a half times its keepalive, has its will published as a broker would: recorded
with the rest, and kept if retained. After DISCONNECT the will is dropped.

The webhook receiver answers every POST with --webhook-status (200) and records its
path, Content-Type and JSON body.

It prints one line per event: `mqtt: connect ...`, `mqtt: publish <topic> [retained]
<payload>`, `mqtt: lost <client>, will <topic> published`, `webhook: POST <path> ...`.
Passwords are never printed, only whether one was given.

As a library (tools/test_ha_fake_broker.py, tools/test_home_assistant.py), FakeBroker
starts both on free ports (port 0) and gives thread-safe views of what arrived:
publishes(), latest(topic), retained(topic), connects(), disconnects(), webhooks(),
and wait_for(predicate, timeout).

Standard library only.
"""

import argparse
import datetime
import http.server
import json
import socket
import sys
import threading
import time
from dataclasses import dataclass
from typing import Optional

HOST = "127.0.0.1"

# packet types (the fixed header's high nibble)
CONNECT, CONNACK, PUBLISH, PUBACK, PUBREC, PUBREL, PUBCOMP = 1, 2, 3, 4, 5, 6, 7
SUBSCRIBE, SUBACK, UNSUBSCRIBE, UNSUBACK, PINGREQ, PINGRESP, DISCONNECT = 8, 9, 10, 11, 12, 13, 14

# CONNACK return codes (MQTT 3.1.1, 3.2.2.3)
ACCEPTED = 0
UNACCEPTABLE_PROTOCOL = 1
BAD_CREDENTIALS = 4


class ProtocolError(Exception):
    """A packet a broker would close the connection over."""


@dataclass
class Connect:
    client_id: str
    clean_session: bool
    keepalive_s: int
    username: Optional[str]
    password: Optional[str]
    will_topic: Optional[str]
    will_payload: Optional[bytes]
    will_retain: bool
    will_qos: int
    protocol: str = "MQTT"
    level: int = 4
    return_code: int = -1  # what the broker answered; set once it has
    time: float = 0.0


@dataclass
class Publish:
    topic: str
    payload: bytes
    retain: bool
    qos: int
    client_id: str
    will: bool = False  # the broker published it as the client's will
    time: float = 0.0

    @property
    def text(self):
        return self.payload.decode("utf-8", errors="replace")


@dataclass
class Disconnect:
    client_id: str
    clean: bool  # the client sent DISCONNECT; otherwise the connection was lost
    reason: str
    time: float = 0.0


@dataclass
class Webhook:
    path: str
    content_type: str
    body: bytes
    json: object  # the body parsed, None when it isn't JSON
    time: float = 0.0


# packet coding

def encode_remaining_length(length):
    out = bytearray()
    while True:
        byte = length % 128
        length //= 128
        out.append(byte | 0x80 if length else byte)
        if not length:
            return bytes(out)


def encode_packet(packet_type, flags, body=b""):
    return bytes([packet_type << 4 | flags]) + encode_remaining_length(len(body)) + body


def encode_string(text):
    data = text.encode("utf-8") if isinstance(text, str) else bytes(text)
    return len(data).to_bytes(2, "big") + data


def encode_connack(return_code, session_present=False):
    return encode_packet(CONNACK, 0, bytes([1 if session_present else 0, return_code]))


class PacketReader:
    """Bytes in any pieces -> whole packets, as (type, flags, body)."""

    def __init__(self):
        self.buffer = bytearray()

    def add(self, data):
        self.buffer += data

    def next(self):
        """The next whole packet, or None until more bytes come. ProtocolError for a
        remaining length longer than four bytes."""
        if len(self.buffer) < 2:
            return None
        length, multiplier, used = 0, 1, 1
        while True:
            if used > 4:
                raise ProtocolError("remaining length longer than 4 bytes")
            if used >= len(self.buffer):
                return None
            byte = self.buffer[used]
            length += (byte & 0x7F) * multiplier
            multiplier *= 128
            used += 1
            if not byte & 0x80:
                break
        if len(self.buffer) < used + length:
            return None
        first = self.buffer[0]
        body = bytes(self.buffer[used:used + length])
        del self.buffer[:used + length]
        return first >> 4, first & 0x0F, body


class _Body:
    """Reads a packet's body field by field."""

    def __init__(self, data):
        self.data = data
        self.at = 0

    def take(self, count):
        if self.at + count > len(self.data):
            raise ProtocolError("packet ends early")
        part = self.data[self.at:self.at + count]
        self.at += count
        return part

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return int.from_bytes(self.take(2), "big")

    def binary(self):
        return self.take(self.u16())

    def string(self):
        try:
            return self.binary().decode("utf-8")
        except UnicodeDecodeError:
            raise ProtocolError("a string that isn't UTF-8")

    def rest(self):
        part = self.data[self.at:]
        self.at = len(self.data)
        return part


def decode_connect(body):
    """A CONNECT's body as a Connect. ProtocolError if it's malformed."""
    b = _Body(body)
    protocol = b.string()
    level = b.u8()
    flags = b.u8()
    keepalive = b.u16()
    if flags & 0x01:
        raise ProtocolError("CONNECT's reserved flag is set")
    client_id = b.string()
    will = bool(flags & 0x04)
    will_topic = b.string() if will else None
    will_payload = bytes(b.binary()) if will else None
    username = b.string() if flags & 0x80 else None
    password = b.binary().decode("utf-8", errors="replace") if flags & 0x40 else None
    return Connect(client_id=client_id, clean_session=bool(flags & 0x02), keepalive_s=keepalive,
                   username=username, password=password, will_topic=will_topic,
                   will_payload=will_payload, will_retain=will and bool(flags & 0x20),
                   will_qos=(flags >> 3) & 0x03 if will else 0, protocol=protocol, level=level)


def decode_publish(flags, body):
    """A PUBLISH as (topic, payload, retain, qos, packet id or None)."""
    qos = (flags >> 1) & 0x03
    if qos == 3:
        raise ProtocolError("PUBLISH with QoS 3")
    b = _Body(body)
    topic = b.string()
    packet_id = b.u16() if qos else None
    return topic, bytes(b.rest()), bool(flags & 0x01), qos, packet_id


def _shown(payload, limit=200):
    text = payload.decode("utf-8", errors="replace")
    return text if len(text) <= limit else text[:limit] + f"... ({len(payload)} bytes)"


class FakeBroker:
    """The broker on mqtt_port and the webhook receiver on webhook_port (0: a free port
    each, read back from .mqtt_port and .webhook_port after start(); webhook_port None:
    no webhook receiver). connack_code answers every CONNECT; with username or password
    given, a client whose own don't match gets 4 (bad user name or password) instead."""

    def __init__(self, mqtt_port=0, webhook_port=0, connack_code=ACCEPTED, username=None,
                 password=None, webhook_status=200, log=None, host=HOST):
        self.host = host
        self.mqtt_port = mqtt_port
        self.webhook_port = webhook_port
        self.connack_code = connack_code
        self.username = username
        self.password = password
        self.webhook_status = webhook_status
        self.log = log or (lambda line: None)
        self._changed = threading.Condition(threading.RLock())
        self._publishes = []
        self._retained = {}
        self._connects = []
        self._disconnects = []
        self._webhooks = []
        self._stopping = False
        self._listener = None
        self._http = None
        self._threads = []
        self._clients = set()

    # life

    def start(self):
        self._listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self._listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self._listener.bind((self.host, self.mqtt_port))
        self._listener.listen(8)
        self.mqtt_port = self._listener.getsockname()[1]
        self._spawn(self._accept_loop)
        if self.webhook_port is not None:
            self._http = http.server.ThreadingHTTPServer((self.host, self.webhook_port),
                                                         self._webhook_handler())
            self._http.daemon_threads = True
            self.webhook_port = self._http.server_address[1]
            self._spawn(self._http.serve_forever, poll_interval=0.1)
        return self

    def stop(self):
        """Closes everything; connections it cuts don't publish their wills."""
        with self._changed:
            self._stopping = True
            clients = list(self._clients)
        if self._listener:
            self._listener.close()
        for sock in clients:
            try:
                sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()
        if self._http:
            self._http.shutdown()
            self._http.server_close()
        for thread in self._threads:
            thread.join(timeout=2)

    def __enter__(self):
        return self.start()

    def __exit__(self, *exc):
        self.stop()

    @property
    def webhook_url_base(self):
        return f"http://{self.host}:{self.webhook_port}"

    # what arrived; each a copy, safe from any thread

    def publishes(self, topic=None):
        """Every PUBLISH so far (the wills it published too), in arrival order; with a
        topic, only that topic's."""
        with self._changed:
            return [p for p in self._publishes if topic is None or p.topic == topic]

    def latest(self, topic):
        """The last payload published to topic, as text, or None."""
        found = self.publishes(topic)
        return found[-1].text if found else None

    def retained(self, topic=None):
        """A retained topic's payload as text (None if nothing is retained there); with
        no topic, every retained topic -> payload text."""
        with self._changed:
            if topic is None:
                return {t: p.decode("utf-8", errors="replace") for t, p in self._retained.items()}
            payload = self._retained.get(topic)
            return None if payload is None else payload.decode("utf-8", errors="replace")

    def connects(self):
        with self._changed:
            return list(self._connects)

    def disconnects(self):
        with self._changed:
            return list(self._disconnects)

    def webhooks(self):
        with self._changed:
            return list(self._webhooks)

    def wait_for(self, predicate, timeout):
        """Waits until predicate() is truthy (checked whenever something arrives, and
        every 0.25 s); returns its value, or the last falsy one at the timeout."""
        deadline = time.monotonic() + timeout
        with self._changed:
            while True:
                value = predicate()
                if value:
                    return value
                left = deadline - time.monotonic()
                if left <= 0:
                    return value
                self._changed.wait(min(left, 0.25))

    # recording

    def _record(self, kind, item):
        with self._changed:
            item.time = time.time()
            if kind == "publish":
                self._publishes.append(item)
                if item.retain:
                    if item.payload:
                        self._retained[item.topic] = item.payload
                    else:
                        self._retained.pop(item.topic, None)
            elif kind == "connect":
                self._connects.append(item)
            elif kind == "disconnect":
                self._disconnects.append(item)
            elif kind == "webhook":
                self._webhooks.append(item)
            self._changed.notify_all()

    def _spawn(self, target, *args, **kwargs):
        thread = threading.Thread(target=target, args=args, kwargs=kwargs, daemon=True)
        thread.start()
        self._threads.append(thread)
        return thread

    # MQTT

    def _accept_loop(self):
        while True:
            try:
                sock, peer = self._listener.accept()
            except OSError:
                return
            with self._changed:
                if self._stopping:
                    sock.close()
                    return
                self._clients.add(sock)
            self._spawn(self._serve_client, sock, peer)

    def _answer_connect(self, connect):
        if self.connack_code != ACCEPTED:
            return self.connack_code
        if connect.protocol != "MQTT" or connect.level != 4:
            return UNACCEPTABLE_PROTOCOL
        if self.username is not None or self.password is not None:
            if connect.username != self.username or connect.password != self.password:
                return BAD_CREDENTIALS
        return ACCEPTED

    def _serve_client(self, sock, peer):
        reader = PacketReader()
        connect = None
        clean = False
        reason = "closed"
        try:
            sock.settimeout(10)  # the CONNECT has to come in good time
            while True:
                packet = reader.next()
                if packet is None:
                    try:
                        data = sock.recv(65536)
                    except socket.timeout:
                        reason = "keepalive ran out" if connect else "no CONNECT"
                        break
                    if not data:
                        reason = "closed without DISCONNECT"
                        break
                    reader.add(data)
                    continue
                kind, flags, body = packet
                if connect is None:
                    if kind != CONNECT:
                        reason = f"packet type {kind} before CONNECT"
                        break
                    connect = decode_connect(body)
                    connect.return_code = self._answer_connect(connect)
                    self._record("connect", connect)
                    self.log(f"mqtt: connect {connect.client_id!r} from {peer[0]}:{peer[1]}"
                             f" keepalive {connect.keepalive_s}"
                             + (f" user {connect.username!r}" if connect.username is not None else "")
                             + (" with a password" if connect.password is not None else "")
                             + (f" will {connect.will_topic} = {_shown(connect.will_payload)!r}"
                                + (" (retained)" if connect.will_retain else "")
                                if connect.will_topic is not None else "")
                             + f" -> connack {connect.return_code}")
                    sock.sendall(encode_connack(connect.return_code))
                    if connect.return_code != ACCEPTED:
                        # a refused client's will is never published
                        clean = True
                        reason = f"refused with {connect.return_code}"
                        break
                    sock.settimeout(connect.keepalive_s * 1.5 if connect.keepalive_s else None)
                elif kind == CONNECT:
                    reason = "a second CONNECT"
                    break
                elif kind == PUBLISH:
                    topic, payload, retain, qos, packet_id = decode_publish(flags, body)
                    self._record("publish", Publish(topic, payload, retain, qos, connect.client_id))
                    self.log(f"mqtt: publish {topic}" + (" [retained]" if retain else "")
                             + (f" qos {qos}" if qos else "") + f" {_shown(payload)!r}")
                    if qos == 1:
                        sock.sendall(encode_packet(PUBACK, 0, packet_id.to_bytes(2, "big")))
                    elif qos == 2:
                        sock.sendall(encode_packet(PUBREC, 0, packet_id.to_bytes(2, "big")))
                elif kind == PUBREL:
                    sock.sendall(encode_packet(PUBCOMP, 0, body[:2]))
                elif kind == SUBSCRIBE:
                    b = _Body(body)
                    packet_id = b.take(2)
                    granted = bytearray()
                    while b.at < len(body):
                        b.string()
                        b.u8()
                        granted.append(0)
                    sock.sendall(encode_packet(SUBACK, 0, packet_id + bytes(granted)))
                elif kind == UNSUBSCRIBE:
                    sock.sendall(encode_packet(UNSUBACK, 0, body[:2]))
                elif kind == PINGREQ:
                    sock.sendall(encode_packet(PINGRESP, 0))
                elif kind == DISCONNECT:
                    clean = True
                    reason = "DISCONNECT"
                    break
                else:
                    reason = f"unexpected packet type {kind}"
                    break
        except ProtocolError as e:
            reason = f"malformed packet: {e}"
        except OSError as e:
            reason = f"connection lost: {e}"
        finally:
            with self._changed:
                self._clients.discard(sock)
                stopping = self._stopping
            try:
                sock.close()
            except OSError:
                pass
        if connect is None or stopping:
            return
        client = connect.client_id
        self._record("disconnect", Disconnect(client, clean, reason))
        if clean or connect.will_topic is None:
            self.log(f"mqtt: {client!r} gone ({reason})")
            return
        self._record("publish", Publish(connect.will_topic, connect.will_payload,
                                        connect.will_retain, connect.will_qos, client, will=True))
        self.log(f"mqtt: lost {client!r} ({reason}), will {connect.will_topic} = "
                 f"{_shown(connect.will_payload)!r} published"
                 + (" (retained)" if connect.will_retain else ""))

    # webhook

    def _webhook_handler(self):
        broker = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def answer(self, status):
                body = b"{}" if status < 300 else b'{"error":"refused by the fake"}'
                self.send_response(status)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(body)
                self.close_connection = True

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0") or 0)
                body = self.rfile.read(length) if length else b""
                try:
                    parsed = json.loads(body.decode("utf-8"))
                except ValueError:
                    parsed = None
                content_type = self.headers.get("Content-Type", "")
                broker._record("webhook", Webhook(self.path, content_type, body, parsed))
                broker.log(f"webhook: POST {self.path} ({content_type or 'no Content-Type'}) "
                           + (json.dumps(parsed) if parsed is not None
                              else f"not JSON: {_shown(body)!r}")
                           + f" -> {broker.webhook_status}")
                self.answer(broker.webhook_status)

            def do_GET(self):
                broker.log(f"webhook: GET {self.path} -> 405")
                self.answer(405)

            def log_message(self, *args):
                pass

        return Handler


def main(argv):
    parser = argparse.ArgumentParser(
        description="A fake MQTT broker and webhook receiver on 127.0.0.1, for testing "
                    "band3's Home Assistant link.")
    parser.add_argument("--mqtt-port", type=int, default=1883, help="0: a free port")
    parser.add_argument("--webhook-port", type=int, default=8124,
                        help="0: a free port; -1: no webhook receiver")
    parser.add_argument("--connack-code", type=int, default=ACCEPTED,
                        help="CONNACK's return code for every client (4: bad user name or "
                             "password, 5: not authorized, 3: server unavailable)")
    parser.add_argument("--username", help="refuse (code 4) clients without this user name")
    parser.add_argument("--password", help="refuse (code 4) clients without this password")
    parser.add_argument("--webhook-status", type=int, default=200,
                        help="the HTTP status the webhook answers with")
    args = parser.parse_args(argv)

    def log(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{stamp} {line}", flush=True)

    broker = FakeBroker(mqtt_port=args.mqtt_port,
                        webhook_port=None if args.webhook_port < 0 else args.webhook_port,
                        connack_code=args.connack_code, username=args.username,
                        password=args.password, webhook_status=args.webhook_status, log=log)
    try:
        broker.start()
    except OSError as e:
        print(f"can't listen on {HOST}: {e}", file=sys.stderr)
        return 1
    log(f"listening on mqtt://{HOST}:{broker.mqtt_port}"
        + (f", webhook {broker.webhook_url_base}/api/webhook/<id>"
           if broker.webhook_port is not None else ""))
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        broker.stop()
        retained = broker.retained()
        if retained:
            log(f"{len(retained)} retained topics at the end:")
            for topic, payload in sorted(retained.items()):
                log(f"  {topic} = {_shown(payload.encode())!r}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
