"""Tests for the mock Liveless Rooms server: python tools/test_liveless_rooms_mock.py

The codec against the golden vectors band3's C++ codec is tested on, then scripted
clients that talk to a server on an ephemeral 127.0.0.1 port the way RB3Enhanced
does: one recv() per frame.
"""

import hashlib
import hmac
import os
import select
import socket
import threading
import time
import unittest

import liveless_rooms_mock as rooms

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN = os.path.join(REPO, "tests", "golden", "liveless_rooms_packets.txt")

KEY = bytes(range(16))
HOST_XUID = 0x0009000000000001


def read_golden():
    vectors = {}
    with open(GOLDEN, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                name, value = line.split()
                vectors[name] = bytes.fromhex(value)
    return vectors


def split_frame(data):
    """A whole frame as (type, body), checking the header's size against it."""
    type_, size = rooms.parse_header(data)
    if len(data) != rooms.HEADER_SIZE + size:
        raise AssertionError(f"frame of {len(data)} bytes says {size} body bytes")
    return type_, data[rooms.HEADER_SIZE:]


class GoldenTest(unittest.TestCase):
    """Every golden frame decodes to the inputs the file names and encodes back."""

    @classmethod
    def setUpClass(cls):
        cls.golden = read_golden()

    def roundtrip(self, name, expected_type, decode, encode):
        type_, body = split_frame(self.golden[name])
        self.assertEqual(type_, expected_type)
        message = decode(body)
        self.assertIsNotNone(message)
        self.assertEqual(encode(message), self.golden[name])
        # a body one byte short isn't the packet
        if body:
            self.assertIsNone(decode(body[:-1]))
        return message

    def test_client_hello(self):
        hello = self.roundtrip("client_hello", rooms.CLIENT_HELLO,
                               rooms.decode_client_hello, rooms.encode_client_hello)
        self.assertEqual(hello, rooms.ClientHello(True, "eng", "band3-test"))

    def test_server_hello(self):
        hello = self.roundtrip("server_hello", rooms.SERVER_HELLO,
                               rooms.decode_server_hello, rooms.encode_server_hello)
        self.assertEqual(hello, rooms.ServerHello(True, True, KEY))

    def test_client_login(self):
        login = self.roundtrip("client_login", rooms.CLIENT_LOGIN,
                               rooms.decode_client_login, rooms.encode_client_login)
        self.assertEqual((login.xuid, login.gamertag, login.local_ipv4),
                         (HOST_XUID, "host", "192.168.1.2"))
        self.assertEqual(len(login.proof), rooms.PROOF_SIZE)
        self.assertEqual(login.proof[:20], self.golden["proof"])
        self.assertTrue(rooms.proof_ok(KEY, "127.0.0.1", login))
        self.assertFalse(rooms.proof_ok(KEY, "127.0.0.2", login))
        # the hash alone encodes to the same frame, zero-padded
        self.assertEqual(rooms.encode_client_login(login._replace(proof=self.golden["proof"])),
                         self.golden["client_login"])

    def test_server_logged_in(self):
        logged_in = self.roundtrip("server_logged_in", rooms.LOGGED_IN,
                                   rooms.decode_server_logged_in, rooms.encode_server_logged_in)
        self.assertEqual(logged_in, rooms.ServerLoggedIn("127.0.0.1", "ABCD2345"))

    def test_join_request(self):
        request = self.roundtrip("join_request", rooms.JOIN_REQUEST,
                                 rooms.decode_join_request, rooms.encode_join_request)
        self.assertEqual(request, rooms.JoinRequest("ABCD2345"))

    def test_join_response(self):
        response = self.roundtrip("join_response", rooms.JOIN_RESPONSE,
                                  rooms.decode_join_response, rooms.encode_join_response)
        self.assertEqual(response, rooms.JoinResponse(0, "host", HOST_XUID,
                                                      "127.0.0.1", "192.168.1.2"))

    def test_join_denied(self):
        denied = self.roundtrip("join_denied", rooms.JOIN_DENIED,
                                rooms.decode_join_denied, rooms.encode_join_denied)
        self.assertEqual(denied, rooms.JoinDenied(rooms.DENIED_CODE_NOT_FOUND))

    def test_nat_punch(self):
        punch = self.roundtrip("nat_punch", rooms.NAT_PUNCH,
                               rooms.decode_nat_punch, rooms.encode_nat_punch)
        self.assertEqual(punch, rooms.NatPunchRequest("127.0.0.1"))

    def test_ping_and_pong_are_the_same_bytes(self):
        self.assertEqual(rooms.PING_FRAME, self.golden["ping"])
        self.assertEqual(rooms.PONG_FRAME, self.golden["pong"])
        self.assertEqual(split_frame(rooms.PING_FRAME), (rooms.PING, b""))
        self.assertEqual(rooms.PING, rooms.PONG)

    def test_proof(self):
        self.assertEqual(rooms.login_proof(KEY, "127.0.0.1", HOST_XUID), self.golden["proof"])

    def test_xuids(self):
        for name in ("host", "joiner", "User"):
            self.assertEqual(rooms.rooms_xuid(name).to_bytes(8, "big"),
                             self.golden["xuid_" + name], name)

    def test_sha1_vectors(self):
        # the mock uses hashlib; these pin the vectors the C++ SHA-1 is held to
        self.assertEqual(hashlib.sha1(b"abc").digest(), self.golden["sha1_abc"])
        self.assertEqual(hmac.new(b"\x0b" * 20, b"Hi There", hashlib.sha1).digest(),
                         self.golden["hmac_rfc2202_1"])
        self.assertEqual(hmac.new(b"Jefe", b"what do ya want for nothing?",
                                  hashlib.sha1).digest(),
                         self.golden["hmac_rfc2202_2"])


class CodecTest(unittest.TestCase):
    def test_strings_are_cut_to_their_fields(self):
        frame = rooms.encode_client_login(rooms.ClientLogin(1, "x" * 20, "10.0.0.1", b""))
        login = rooms.decode_client_login(split_frame(frame)[1])
        self.assertEqual(login.gamertag, "x" * 15)
        self.assertEqual(rooms.decode_client_hello(split_frame(rooms.encode_client_hello(
            rooms.ClientHello(True, "en", "v" * 60)))[1]), rooms.ClientHello(True, "en", "v" * 47))

    def test_a_short_code_is_nul_padded(self):
        self.assertEqual(rooms.encode_join_request(rooms.JoinRequest("ab")),
                         bytes.fromhex("4C4C00030008") + b"ab" + bytes(6))

    def test_a_longer_body_is_accepted(self):
        self.assertEqual(rooms.decode_join_denied(b"\x00extra"), rooms.JoinDenied(0))

    def test_frame_reader_splits_and_joins(self):
        golden = read_golden()
        stream = golden["client_hello"] + golden["ping"] + golden["join_request"]
        reader = rooms.FrameReader()
        frames = []
        for i in range(0, len(stream), 7):
            reader.feed(stream[i:i + 7])
            while (f := reader.next()) is not None:
                frames.append(f)
        self.assertEqual([t for t, _ in frames], [rooms.CLIENT_HELLO, rooms.PONG, rooms.JOIN_REQUEST])
        self.assertEqual(frames[2][1], b"ABCD2345")
        self.assertFalse(reader.broken)

    def test_frame_reader_breaks_on_bad_magic_or_size(self):
        reader = rooms.FrameReader()
        reader.feed(b"LX\x00\x00\x00\x00")
        self.assertIsNone(reader.next())
        self.assertTrue(reader.broken)
        reader = rooms.FrameReader()
        reader.feed(bytes.fromhex("4C4C00010301"))
        self.assertIsNone(reader.next())
        self.assertTrue(reader.broken)


class Client:
    """A scripted Liveless client, reading as RB3Enhanced does: one recv() per frame."""

    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def send(self, data):
        self.sock.sendall(data)

    def recv(self, timeout=5):
        """The next frame as (type, body), or None once the server closes.

        One recv() must hold exactly one frame: RB3E reads once per game frame
        and drops whatever follows the first packet in its buffer."""
        self.sock.settimeout(timeout)
        try:
            data = self.sock.recv(rooms.HEADER_SIZE + rooms.MAX_BODY)
        except ConnectionResetError:
            return None
        if not data:
            return None
        return split_frame(data)

    def expect(self, type_, decode, timeout=5):
        frame = self.recv(timeout)
        if frame is None:
            raise AssertionError(f"server closed while waiting for type {type_}")
        if frame[0] != type_:
            raise AssertionError(f"wanted type {type_}, got {frame}")
        return decode(frame[1])

    def hello(self, emulator=True):
        self.send(rooms.encode_client_hello(rooms.ClientHello(emulator, "eng", "band3-test")))
        return self.expect(rooms.SERVER_HELLO, rooms.decode_server_hello)

    def login(self, gamertag, local_ipv4, address="127.0.0.1", emulator=True):
        server_hello = self.hello(emulator)
        xuid = rooms.rooms_xuid(gamertag)
        proof = (rooms.login_proof(server_hello.proof_key, address, xuid)
                 if server_hello.needs_proof else b"")
        self.send(rooms.encode_client_login(rooms.ClientLogin(xuid, gamertag, local_ipv4, proof)))
        return self.expect(rooms.LOGGED_IN, rooms.decode_server_logged_in)

    def close(self):
        self.sock.close()


class ServerTest(unittest.TestCase):
    def start(self, **options):
        options.setdefault("ping", 0)
        self.log = []
        server = rooms.RoomsServer(port=0, log=self.log.append, **options)
        thread = threading.Thread(target=server.serve_forever, daemon=True)
        thread.start()

        def stop():
            server.stop()
            thread.join(5)
            server.close()
        self.addCleanup(stop)
        return server.port

    def connect(self, port):
        client = Client(port)
        self.addCleanup(client.close)
        return client

    def wait_log(self, text, timeout=5):
        """The first log line holding text, waiting for the server thread to write it."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            for line in list(self.log):
                if text in line:
                    return line
            time.sleep(0.02)
        self.fail(f"no log line with {text!r} in {self.log}")


class SessionTest(ServerTest):
    def test_host_and_joiner(self):
        port = self.start(address="127.0.0.1", codes=["HOST0001", "JOIN0001"])
        host = self.connect(port)
        joiner = self.connect(port)

        server_hello = host.hello()
        self.assertEqual((server_hello.allowed, server_hello.needs_proof), (True, True))
        self.assertEqual(len(server_hello.proof_key), 16)
        xuid = rooms.rooms_xuid("host")
        self.send_login(host, server_hello, xuid)
        logged_in = host.expect(rooms.LOGGED_IN, rooms.decode_server_logged_in)
        self.assertEqual(logged_in, rooms.ServerLoggedIn("127.0.0.1", "HOST0001"))
        self.wait_log("code HOST0001")

        self.assertEqual(joiner.login("joiner", "192.168.1.3"),
                         rooms.ServerLoggedIn("127.0.0.1", "JOIN0001"))

        # the code as typed, lower case
        joiner.send(rooms.encode_join_request(rooms.JoinRequest("host0001")))
        self.assertEqual(host.expect(rooms.NAT_PUNCH, rooms.decode_nat_punch),
                         rooms.NatPunchRequest("127.0.0.1"))
        self.assertEqual(joiner.expect(rooms.JOIN_RESPONSE, rooms.decode_join_response),
                         rooms.JoinResponse(0, "host", xuid, "127.0.0.1", "192.168.1.2"))
        self.wait_log("join #2")

        joiner.send(rooms.encode_join_request(rooms.JoinRequest("NOPE2345")))
        self.assertEqual(joiner.expect(rooms.JOIN_DENIED, rooms.decode_join_denied),
                         rooms.JoinDenied(0))
        joiner.send(rooms.encode_join_request(rooms.JoinRequest("JOIN0001")))
        self.assertEqual(joiner.expect(rooms.JOIN_DENIED, rooms.decode_join_denied),
                         rooms.JoinDenied(0))
        self.wait_log("deny #2")

    def send_login(self, client, server_hello, xuid):
        proof = rooms.login_proof(server_hello.proof_key, "127.0.0.1", xuid)
        frame = rooms.encode_client_login(rooms.ClientLogin(xuid, "host", "192.168.1.2", proof))
        # the exact bytes RB3E sends: XUID, gamertag, LAN address, then the hash
        self.assertEqual(len(frame), 6 + 580)
        self.assertEqual(frame[6:14], xuid.to_bytes(8, "big"))
        self.assertEqual(frame[30:34], bytes([192, 168, 1, 2]))
        self.assertEqual(frame[34:54], proof)
        client.send(frame)

    def test_a_bad_proof_is_closed_without_logging_in(self):
        port = self.start(address="127.0.0.1")
        client = self.connect(port)
        server_hello = client.hello()
        xuid = rooms.rooms_xuid("host")
        # signed for another server's address
        proof = rooms.login_proof(server_hello.proof_key, "liveless.example", xuid)
        client.send(rooms.encode_client_login(rooms.ClientLogin(xuid, "host", "10.0.0.2", proof)))
        self.assertIsNone(client.recv())
        self.wait_log("bad proof")

    def test_without_proofs_any_login_is_taken(self):
        port = self.start(proof=False, public="203.0.113.7", codes=["ABCD2345"])
        client = self.connect(port)
        server_hello = client.hello()
        self.assertEqual((server_hello.allowed, server_hello.needs_proof), (True, False))
        client.send(rooms.encode_client_login(rooms.ClientLogin(5, "host", "10.0.0.2", b"")))
        self.assertEqual(client.expect(rooms.LOGGED_IN, rooms.decode_server_logged_in),
                         rooms.ServerLoggedIn("203.0.113.7", "ABCD2345"))

    def test_a_console_login_is_taken_unless_strict(self):
        port = self.start()
        client = self.connect(port)
        client.hello(emulator=False)
        # a console signs the hash; the mock can't check that, so it takes it
        client.send(rooms.encode_client_login(rooms.ClientLogin(5, "box", "10.0.0.2", b"\x01" * 0x228)))
        self.assertIsNotNone(client.expect(rooms.LOGGED_IN, rooms.decode_server_logged_in))

        port = self.start(strict=True)
        client = self.connect(port)
        client.hello(emulator=False)
        client.send(rooms.encode_client_login(rooms.ClientLogin(5, "box", "10.0.0.2", b"\x01" * 0x228)))
        self.assertIsNone(client.recv())

    def test_codes_are_random_without_codes(self):
        port = self.start()
        code = self.connect(port).login("host", "10.0.0.2").code
        self.assertEqual(len(code), 8)
        self.assertTrue(set(code) <= set(rooms.CODE_ALPHABET), code)

    def test_anything_but_a_hello_first_is_closed(self):
        port = self.start()
        client = self.connect(port)
        client.send(rooms.encode_join_request(rooms.JoinRequest("ABCD2345")))
        self.assertIsNone(client.recv())
        client = self.connect(port)
        client.send(b"XX\x00\x00\x00\x00")
        self.assertIsNone(client.recv())

    def test_a_login_in_two_halves_is_put_back_together(self):
        port = self.start(codes=["HOST0001"])
        client = self.connect(port)
        server_hello = client.hello()
        xuid = rooms.rooms_xuid("host")
        frame = rooms.encode_client_login(rooms.ClientLogin(
            xuid, "host", "192.168.1.2", rooms.login_proof(server_hello.proof_key, "127.0.0.1", xuid)))
        # the first half ends inside the header's size field
        client.send(frame[:5])
        time.sleep(0.15)
        client.send(frame[5:300])
        time.sleep(0.15)
        client.send(frame[300:])
        self.assertEqual(client.expect(rooms.LOGGED_IN, rooms.decode_server_logged_in).code, "HOST0001")


class PingTest(ServerTest):
    def test_a_pong_keeps_a_client_and_silence_drops_it(self):
        port = self.start(ping=1, drop=2)
        answering = self.connect(port)
        silent = self.connect(port)
        answering.login("host", "10.0.0.2")
        silent.login("joiner", "10.0.0.3")

        # both are read as their frames come, as two games would, so neither
        # one's pings pile up into a read that holds two
        open_clients = {answering.sock: answering, silent.sock: silent}
        answered = silent_pings = 0
        silent_closed_after = None
        started = time.monotonic()
        while time.monotonic() < started + 3.6:
            readable, _, _ = select.select(list(open_clients), [], [], 0.25)
            for sock in readable:
                frame = open_clients[sock].recv()
                if sock is answering.sock:
                    self.assertEqual(frame, (rooms.PING, b""), "the client that pongs")
                    answering.send(rooms.PONG_FRAME)
                    answered += 1
                elif frame is None:
                    silent_closed_after = time.monotonic() - started
                    del open_clients[sock]
                else:
                    self.assertEqual(frame, (rooms.PING, b""))
                    silent_pings += 1
        self.assertGreaterEqual(answered, 3)
        self.assertGreaterEqual(silent_pings, 1)
        self.assertIsNotNone(silent_closed_after, "the silent client was never closed")
        self.assertLess(silent_closed_after, 3.0)
        self.wait_log("ping timeout #2")
        self.assertFalse(any("close #1" in line for line in self.log), self.log)


class OneFramePerReadTest(ServerTest):
    def test_frames_queued_together_arrive_apart(self):
        port = self.start(ping=1, codes=["HOST0001", "JOIN0001"])
        host = self.connect(port)
        joiner = self.connect(port)
        host.login("host", "192.168.1.2")
        joiner.login("joiner", "192.168.1.3")

        # three requests in one segment: the server has two NAT punches for the host
        # and three replies for the joiner at the same moment, with pings due soon
        request = rooms.encode_join_request(rooms.JoinRequest("HOST0001"))
        joiner.send(request + request + rooms.encode_join_request(rooms.JoinRequest("NOPE2345")))

        # Both read as their frames come, as two games would; Client.recv fails if
        # a read holds more (or less) than one frame. Through the first ping each.
        types = {host.sock: [], joiner.sock: []}
        clients = {host.sock: host, joiner.sock: joiner}
        deadline = time.monotonic() + 4
        while time.monotonic() < deadline and not all(rooms.PING in t for t in types.values()):
            readable, _, _ = select.select(list(clients), [], [], 0.25)
            for sock in readable:
                frame = clients[sock].recv()
                self.assertIsNotNone(frame)
                types[sock].append(frame[0])
        self.assertEqual(types[host.sock], [rooms.NAT_PUNCH, rooms.NAT_PUNCH, rooms.PING])
        self.assertEqual(types[joiner.sock],
                         [rooms.JOIN_RESPONSE, rooms.JOIN_RESPONSE, rooms.JOIN_DENIED, rooms.PING])


if __name__ == "__main__":
    unittest.main()
