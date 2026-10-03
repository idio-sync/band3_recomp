"""liveless_rooms_mock: a stand-in Liveless Rooms server, for testing band3 offline.

RB3Enhanced's Liveless Rooms server hands every logged-in game an 8-character user
code and, when one game asks to join another's code, tells each where to find the
other. This speaks the same protocol (RB3E's net_liveless_online.c, TCP, packed
big-endian structs) on 127.0.0.1 only, so band3's client can be tested without the
public server:

  python tools/liveless_rooms_mock.py                          port 19532, random codes
  python tools/liveless_rooms_mock.py --codes HOST0001,JOIN0001 --hex

For two band3s on this PC, start it with --codes as above, then point both games'
Liveless Rooms server at 127.0.0.1 (the same string as --address: each game signs its
login with the address it was given) and give the second game its own
`liveless_port`. The first game to connect gets HOST0001, the second JOIN0001; the
joiner enters host0001. Both games show as 127.0.0.1 to the mock, so the joiner is
sent to the host's LAN address, as two games behind one router would be.

It logs one line per event (connect, hello, login with its code, join, deny, ping
timeout, close), and with --hex every frame in and out. --ping and --drop take
seconds; 0 turns pinging (and so dropping) off.

The packing functions (encode_*/decode_*, login_proof, rooms_xuid) are kept apart
from the server for tools/test_liveless_rooms_mock.py, which checks them against
tests/golden/liveless_rooms_packets.txt, the vectors band3's C++ codec is held to.

Standard library only.
"""

import argparse
import collections
import datetime
import hashlib
import hmac
import os
import secrets
import selectors
import socket
import struct
import sys
import time

PORT = 19532
MAGIC = 0x4C4C
PROTOCOL_VERSION = 0
HEADER_SIZE = 6
MAX_BODY = 0x300
PROOF_SIZE = 0x228
HASH_SIZE = 20
CODE_ALPHABET = "ABCDEFGHJKLMNPQRSTUVWXYZ23456789"

# client -> server
CLIENT_HELLO, CLIENT_LOGIN, PONG, JOIN_REQUEST = 0, 1, 2, 3
# server -> client; the numbers overlap the other direction's
SERVER_HELLO, LOGGED_IN, PING, JOIN_RESPONSE, JOIN_DENIED, NAT_PUNCH = 0, 1, 2, 3, 4, 5

JOIN_BY_CODE = 0
DENIED_CODE_NOT_FOUND = 0

# RB3E reads its socket once per game frame and takes one packet from what it got,
# so two frames that arrive together lose the second. The mock sends each frame on
# its own and waits this long before the next one to the same game.
FRAME_GAP = 0.2

_HEADER = struct.Struct(">HBBH")
_CLIENT_HELLO = struct.Struct(">BB3s48s")
_SERVER_HELLO = struct.Struct(">BB16s")
_CLIENT_LOGIN = struct.Struct(">Q16s4s%ds" % PROOF_SIZE)
_LOGGED_IN = struct.Struct(">4s8s")
_JOIN_REQUEST = struct.Struct(">8s")
_JOIN_RESPONSE = struct.Struct(">B16sQ4s4s")
_JOIN_DENIED = struct.Struct(">B")
_NAT_PUNCH = struct.Struct(">4s")

ClientHello = collections.namedtuple(
    "ClientHello", "emulator language version protocol", defaults=(PROTOCOL_VERSION,))
ServerHello = collections.namedtuple("ServerHello", "allowed needs_proof proof_key")
ClientLogin = collections.namedtuple("ClientLogin", "xuid gamertag local_ipv4 proof")
ServerLoggedIn = collections.namedtuple("ServerLoggedIn", "public_ipv4 code")
JoinRequest = collections.namedtuple("JoinRequest", "code")
JoinResponse = collections.namedtuple(
    "JoinResponse", "join_type user xuid public_ipv4 private_ipv4")
JoinDenied = collections.namedtuple("JoinDenied", "reason")
NatPunchRequest = collections.namedtuple("NatPunchRequest", "public_ipv4")


def frame(type_, body=b""):
    """A whole packet: the 6-byte header (magic, reserved 0, type, body size) and body."""
    return _HEADER.pack(MAGIC, 0, type_, len(body)) + body


# Ping and Pong are the same six bytes, one each way
PING_FRAME = frame(PING)
PONG_FRAME = frame(PONG)


def parse_header(data):
    """(type, body size) from a frame's first 6 bytes; ValueError if they aren't a header."""
    if len(data) < HEADER_SIZE:
        raise ValueError(f"{len(data)} bytes is no header")
    magic, _, type_, size = _HEADER.unpack_from(data)
    if magic != MAGIC:
        raise ValueError(f"bad magic {magic:04X}")
    if size > MAX_BODY:
        raise ValueError(f"body of {size} bytes is over the limit")
    return type_, size


class FrameReader:
    """Cuts a TCP stream into frames on the header's size, however the bytes arrive.

    `broken` once a header has the wrong magic or too big a size: the stream can't
    be resynchronised after that, so the connection should go."""

    def __init__(self):
        self.buffer = bytearray()
        self.broken = False

    def feed(self, data):
        self.buffer += data

    def next(self):
        """The next whole frame as (type, body), or None until more arrives."""
        if self.broken or len(self.buffer) < HEADER_SIZE:
            return None
        try:
            type_, size = parse_header(self.buffer)
        except ValueError:
            self.broken = True
            return None
        if len(self.buffer) < HEADER_SIZE + size:
            return None
        body = bytes(self.buffer[HEADER_SIZE:HEADER_SIZE + size])
        del self.buffer[:HEADER_SIZE + size]
        return type_, body


def _ip(text):
    """A dotted quad as the 4 bytes on the wire, in the same order (in_addr.S_addr)."""
    return socket.inet_aton(text)


def _ip_text(data):
    return socket.inet_ntoa(data)


def _cstr(text, room):
    """text as UTF-8, cut so a NUL still fits in a field of `room` bytes."""
    return text.encode("utf-8")[:room - 1]


def _text(data):
    """A char[] field up to its first NUL (or its end)."""
    return data.split(b"\0", 1)[0].decode("utf-8", errors="replace")


def _unpack(layout, body):
    # a longer body is taken, as RB3E would, but a shorter one isn't the packet
    if len(body) < layout.size:
        return None
    return layout.unpack_from(body)


def encode_client_hello(hello):
    return frame(CLIENT_HELLO, _CLIENT_HELLO.pack(
        hello.protocol, 1 if hello.emulator else 0,
        hello.language.encode("utf-8")[:3], _cstr(hello.version, 0x30)))


def decode_client_hello(body):
    fields = _unpack(_CLIENT_HELLO, body)
    if fields is None:
        return None
    protocol, emulator, language, version = fields
    return ClientHello(emulator != 0, _text(language), _text(version), protocol)


def encode_server_hello(hello):
    return frame(SERVER_HELLO, _SERVER_HELLO.pack(
        1 if hello.allowed else 0, 1 if hello.needs_proof else 0, bytes(hello.proof_key)))


def decode_server_hello(body):
    fields = _unpack(_SERVER_HELLO, body)
    if fields is None:
        return None
    allowed, needs_proof, key = fields
    return ServerHello(allowed != 0, needs_proof != 0, key)


def encode_client_login(login):
    """proof can be the 20-byte hash alone: the rest of the field is zeros."""
    return frame(CLIENT_LOGIN, _CLIENT_LOGIN.pack(
        login.xuid, _cstr(login.gamertag, 16), _ip(login.local_ipv4),
        bytes(login.proof)[:PROOF_SIZE]))


def decode_client_login(body):
    """The proof comes back whole, all 0x228 bytes."""
    fields = _unpack(_CLIENT_LOGIN, body)
    if fields is None:
        return None
    xuid, gamertag, local_ipv4, proof = fields
    return ClientLogin(xuid, _text(gamertag), _ip_text(local_ipv4), proof)


def encode_server_logged_in(logged_in):
    # exactly 8 characters with no NUL after them: RB3E copies the 8 bytes
    return frame(LOGGED_IN, _LOGGED_IN.pack(
        _ip(logged_in.public_ipv4), logged_in.code.encode("ascii")[:8]))


def decode_server_logged_in(body):
    fields = _unpack(_LOGGED_IN, body)
    if fields is None:
        return None
    public_ipv4, code = fields
    return ServerLoggedIn(_ip_text(public_ipv4), _text(code))


def encode_join_request(request):
    return frame(JOIN_REQUEST, _JOIN_REQUEST.pack(request.code.encode("ascii")[:8]))


def decode_join_request(body):
    fields = _unpack(_JOIN_REQUEST, body)
    if fields is None:
        return None
    return JoinRequest(_text(fields[0]))


def encode_join_response(response):
    return frame(JOIN_RESPONSE, _JOIN_RESPONSE.pack(
        response.join_type, _cstr(response.user, 16), response.xuid,
        _ip(response.public_ipv4), _ip(response.private_ipv4)))


def decode_join_response(body):
    fields = _unpack(_JOIN_RESPONSE, body)
    if fields is None:
        return None
    join_type, user, xuid, public_ipv4, private_ipv4 = fields
    return JoinResponse(join_type, _text(user), xuid, _ip_text(public_ipv4),
                        _ip_text(private_ipv4))


def encode_join_denied(denied):
    return frame(JOIN_DENIED, _JOIN_DENIED.pack(denied.reason))


def decode_join_denied(body):
    fields = _unpack(_JOIN_DENIED, body)
    return None if fields is None else JoinDenied(fields[0])


def encode_nat_punch(punch):
    return frame(NAT_PUNCH, _NAT_PUNCH.pack(_ip(punch.public_ipv4)))


def decode_nat_punch(body):
    fields = _unpack(_NAT_PUNCH, body)
    return None if fields is None else NatPunchRequest(_ip_text(fields[0]))


def login_proof(key, address, xuid):
    """What an emulated game puts at the start of its login's proof: HMAC-SHA1 keyed
    with the ServerHello's key over the server address as the player typed it (no
    NUL) and the XUID's 8 big-endian bytes."""
    return hmac.new(bytes(key), address.encode("utf-8") + xuid.to_bytes(8, "big"),
                    hashlib.sha1).digest()


def proof_ok(key, address, login):
    """An emulator's proof: the hash, then nothing but zeros."""
    proof = bytes(login.proof)
    return (proof[:HASH_SIZE] == login_proof(key, address, login.xuid)
            and not any(proof[HASH_SIZE:]))


def rooms_xuid(username):
    """The XUID band3 logs in with: an FNV-1a-64 of the name in the low 48 bits under
    the 0x0009 prefix, never 0 there."""
    value = 0xCBF29CE484222325
    for byte in username.encode("utf-8"):
        value = ((value ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    low = value & 0x0000FFFFFFFFFFFF
    return 0x0009000000000000 | (low or 1)


def random_code(taken=()):
    while True:
        code = "".join(secrets.choice(CODE_ALPHABET) for _ in range(8))
        if code not in taken:
            return code


class Connection:
    def __init__(self, number, sock, peer, code, now):
        self.number = number
        self.sock = sock
        self.peer = peer
        self.code = code
        self.reader = FrameReader()
        self.outbox = collections.deque()
        self.next_send = now
        self.proof_key = os.urandom(16)
        self.hello = None
        self.login = None
        self.public = None
        self.last_pong = now
        self.next_ping = None
        self.closed = False

    @property
    def name(self):
        if self.login is None:
            return f"#{self.number}"
        return f'#{self.number} ("{self.login.gamertag}")'


class RoomsServer:
    """The mock, single-threaded on a selector. serve_forever() until stop()."""

    def __init__(self, port=PORT, address="127.0.0.1", proof=True, public=None, codes=(),
                 ping=10.0, drop=30.0, hex=False, strict=False, host="127.0.0.1", log=print):
        self.address = address
        self.proof = proof
        self.public = public
        self.codes = collections.deque(code.upper() for code in codes)
        self.ping = ping
        self.drop = drop
        self.hex = hex
        self.strict = strict
        self.log = log
        self.connections = []
        self.by_code = {}
        self.count = 0
        self.stopping = False
        self.selector = selectors.DefaultSelector()
        self.listener = socket.create_server((host, port))
        self.listener.setblocking(False)
        self.selector.register(self.listener, selectors.EVENT_READ)
        self.port = self.listener.getsockname()[1]

    def serve_forever(self):
        while not self.stopping:
            for key, _ in self.selector.select(self._timeout()):
                if key.data is None:
                    self._accept()
                elif not key.data.closed:
                    self._read(key.data)
            self._tick()

    def stop(self):
        """Ends serve_forever within a tenth of a second; safe from another thread."""
        self.stopping = True

    def close(self):
        for conn in list(self.connections):
            self._close(conn, "server stopping")
        self.selector.close()
        self.listener.close()

    def _timeout(self):
        # short enough to notice stop(), and no later than the next send or ping
        now = time.monotonic()
        due = now + 0.1
        for conn in self.connections:
            if conn.outbox:
                due = min(due, conn.next_send)
            if conn.next_ping is not None:
                due = min(due, conn.next_ping)
        return max(0.0, due - now)

    def _accept(self):
        try:
            sock, peer = self.listener.accept()
        except BlockingIOError:
            return
        sock.setblocking(False)
        # each frame leaves as its own segment, not held back to join the next
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.count += 1
        # codes go out in connection order, so a test knows who gets which
        code = self.codes.popleft() if self.codes else random_code(
            {c.code for c in self.connections})
        now = time.monotonic()
        conn = Connection(self.count, sock, peer, code, now)
        if self.ping > 0:
            conn.next_ping = now + self.ping
        self.connections.append(conn)
        self.selector.register(sock, selectors.EVENT_READ, conn)
        self.log(f"connect #{conn.number} from {peer[0]}:{peer[1]}")

    def _read(self, conn):
        try:
            data = conn.sock.recv(65536)
        except BlockingIOError:
            return
        except ConnectionResetError:
            # what Windows reports when a game closes with frames still unread
            self._close(conn, "client disconnected (reset)")
            return
        except OSError as e:
            self._close(conn, f"read failed: {e}")
            return
        if not data:
            self._close(conn, "client disconnected")
            return
        conn.reader.feed(data)
        while not conn.closed and (f := conn.reader.next()) is not None:
            if self.hex:
                self.log(f"#{conn.number} <- {frame(*f).hex().upper()}")
            self._handle(conn, *f)
        if not conn.closed and conn.reader.broken:
            self._close(conn, "bad magic or size: " + bytes(conn.reader.buffer[:6]).hex().upper())

    def _handle(self, conn, type_, body):
        if conn.hello is None:
            # the game speaks first, and only to say hello
            hello = decode_client_hello(body) if type_ == CLIENT_HELLO else None
            if hello is None:
                self._close(conn, f"wanted a ClientHello, got type {type_} of {len(body)} bytes")
                return
            conn.hello = hello
            self.log(f"hello #{conn.number}: {'emulator' if hello.emulator else 'console'}, "
                     f'"{hello.language}", "{hello.version}", protocol {hello.protocol}')
            self._send(conn, encode_server_hello(ServerHello(True, self.proof, conn.proof_key)))
        elif type_ == CLIENT_HELLO:
            self.log(f"hello again from {conn.name}, ignored")
        elif type_ == CLIENT_LOGIN:
            self._login(conn, body)
        elif type_ == PONG:
            conn.last_pong = time.monotonic()
        elif type_ == JOIN_REQUEST:
            self._join(conn, body)
        else:
            self.log(f"unknown type {type_} from {conn.name}, ignored")

    def _login(self, conn, body):
        if conn.login is not None:
            self.log(f"login again from {conn.name}, ignored")
            return
        login = decode_client_login(body)
        if login is None:
            self._close(conn, f"short ClientLogin ({len(body)} bytes)")
            return
        # There is no login-failed packet: a game that isn't let in just loses its
        # connection. A console signs the hash with its key, which the mock can't
        # check, so it's taken unless --strict.
        if conn.hello.emulator:
            if self.proof and not proof_ok(conn.proof_key, self.address, login):
                self.log(f'bad proof #{conn.number} ("{login.gamertag}"): not signed for '
                         f'"{self.address}"')
                self._close(conn, "login refused")
                return
        elif self.strict:
            self.log(f'console login #{conn.number} ("{login.gamertag}") refused: --strict')
            self._close(conn, "login refused")
            return
        conn.login = login
        conn.public = self.public or conn.peer[0]
        self.by_code[conn.code] = conn
        self.log(f"login {conn.name}: xuid {login.xuid:016X} local {login.local_ipv4} "
                 f"public {conn.public} code {conn.code}")
        self._send(conn, encode_server_logged_in(ServerLoggedIn(conn.public, conn.code)))

    def _join(self, conn, body):
        request = decode_join_request(body)
        if request is None:
            self._close(conn, f"short UserJoinRequest ({len(body)} bytes)")
            return
        if conn.login is None:
            self.log(f"join from {conn.name} before logging in, ignored")
            return
        # players type the code in either case
        code = request.code.replace("\0", "").upper()
        host = self.by_code.get(code)
        if host is None or host is conn:
            why = "their own code" if host is conn else "no such code"
            self.log(f"deny {conn.name} -> {code}: {why}")
            self._send(conn, encode_join_denied(JoinDenied(DENIED_CODE_NOT_FOUND)))
            return
        # the host first, so its game is punching towards the joiner by the time
        # the joiner's arrives
        self._send(host, encode_nat_punch(NatPunchRequest(conn.public)))
        self._send(conn, encode_join_response(JoinResponse(
            JOIN_BY_CODE, host.login.gamertag, host.login.xuid, host.public,
            host.login.local_ipv4)))
        self.log(f"join {conn.name} -> {code} {host.name}: public {host.public} "
                 f"local {host.login.local_ipv4}")

    def _send(self, conn, data):
        conn.outbox.append(data)
        self._flush(conn, time.monotonic())

    def _flush(self, conn, now):
        if conn.closed or not conn.outbox or now < conn.next_send:
            return
        data = conn.outbox[0]
        try:
            sent = conn.sock.send(data)
        except BlockingIOError:
            return
        except OSError as e:
            self._close(conn, f"send failed: {e}")
            return
        if self.hex and sent == len(data):
            self.log(f"#{conn.number} -> {data.hex().upper()}")
        if sent < len(data):
            # a full socket buffer; RB3E would see this frame in pieces
            self.log(f"{conn.name}: sent {sent} of {len(data)} bytes, the rest later")
            conn.outbox[0] = data[sent:]
            return
        conn.outbox.popleft()
        conn.next_send = now + FRAME_GAP

    def _tick(self):
        now = time.monotonic()
        for conn in list(self.connections):
            if conn.next_ping is not None and now >= conn.next_ping:
                # a game hangs up after 30 s without hearing from the server
                conn.outbox.append(PING_FRAME)
                conn.next_ping = now + self.ping
            if conn.next_ping is not None and self.drop > 0 and now - conn.last_pong > self.drop:
                self.log(f"ping timeout {conn.name}: no pong in {self.drop:g} s")
                self._close(conn, "ping timeout")
                continue
            self._flush(conn, now)

    def _close(self, conn, reason):
        if conn.closed:
            return
        conn.closed = True
        self.selector.unregister(conn.sock)
        conn.sock.close()
        self.connections.remove(conn)
        if self.by_code.get(conn.code) is conn:
            del self.by_code[conn.code]
        self.log(f"close {conn.name}: {reason}")


def parse_codes(text):
    codes = [c.strip().upper() for c in text.split(",") if c.strip()]
    for code in codes:
        if len(code) != 8 or not code.isascii():
            raise argparse.ArgumentTypeError(f"{code!r} isn't 8 characters")
    return codes


def parse_ipv4(text):
    try:
        socket.inet_aton(text)
    except OSError:
        raise argparse.ArgumentTypeError(f"{text!r} isn't an IPv4 address")
    return text


def main(argv):
    parser = argparse.ArgumentParser(
        description="A mock Liveless Rooms server on 127.0.0.1, for testing band3.")
    parser.add_argument("--port", type=int, default=PORT)
    parser.add_argument("--address", default="127.0.0.1",
                        help="the server address the games were given, which their "
                             "login proofs are signed with")
    parser.add_argument("--no-proof", action="store_true",
                        help="don't ask for proofs (ServerHello requires_proof = 0)")
    parser.add_argument("--public", type=parse_ipv4,
                        help="the public IPv4 every game is told it has and is reached "
                             "at (default: its address as the mock sees it)")
    parser.add_argument("--codes", type=parse_codes, default=[],
                        help="user codes to hand out in connection order, e.g. "
                             "HOST0001,JOIN0001; random ones after them")
    parser.add_argument("--ping", type=float, default=10.0,
                        help="seconds between pings to each game (0: none)")
    parser.add_argument("--drop", type=float, default=30.0,
                        help="close a game that hasn't answered a ping in this many "
                             "seconds (0: never)")
    parser.add_argument("--hex", action="store_true", help="log every frame in hex")
    parser.add_argument("--strict", action="store_true",
                        help="refuse console logins, whose signed proofs the mock can't check")
    args = parser.parse_args(argv)
    if args.ping > 0 and 0 < args.drop <= args.ping:
        parser.error("--drop must be longer than --ping, or every game is dropped")

    def log(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{stamp} {line}", flush=True)

    try:
        server = RoomsServer(port=args.port, address=args.address, proof=not args.no_proof,
                             public=args.public, codes=args.codes, ping=args.ping,
                             drop=args.drop, hex=args.hex, strict=args.strict, log=log)
    except OSError as e:
        print(f"can't listen on 127.0.0.1:{args.port}: {e}", file=sys.stderr)
        return 1
    log(f"listening on 127.0.0.1:{server.port}, proofs "
        + (f'signed for "{args.address}"' if server.proof else "off"))
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
