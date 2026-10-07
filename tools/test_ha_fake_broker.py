"""Tests for the fake Home Assistant broker: python tools/test_ha_fake_broker.py

Offline: it doesn't launch the game. A broker and webhook receiver on free 127.0.0.1
ports, driven by a hand-rolled MQTT client (raw socket bytes, as band3's
src/Net/mqtt_protocol.cpp writes them) and urllib for the webhook.
"""

import json
import socket
import time
import unittest
import urllib.error
import urllib.request

import ha_fake_broker as fake


def string(text):
    data = text.encode()
    return len(data).to_bytes(2, "big") + data


def connect_packet(client_id, will=None, username=None, password=None, keepalive=60):
    """CONNECT, MQTT 3.1.1, clean session; will is (topic, payload, retain)."""
    flags = 0x02
    payload = string(client_id)
    if will:
        topic, message, retain = will
        flags |= 0x04 | (0x20 if retain else 0)
        payload += string(topic) + string(message)
    if username is not None:
        flags |= 0x80
        payload += string(username)
        if password is not None:
            flags |= 0x40
            payload += string(password)
    body = string("MQTT") + bytes([4, flags]) + keepalive.to_bytes(2, "big") + payload
    return bytes([0x10]) + fake.encode_remaining_length(len(body)) + body


def publish_packet(topic, payload, retain=False):
    body = string(topic) + payload.encode()
    return bytes([0x30 | (1 if retain else 0)]) + fake.encode_remaining_length(len(body)) + body


PINGREQ = bytes([0xC0, 0x00])
DISCONNECT = bytes([0xE0, 0x00])


class Client:
    """One raw TCP connection to the broker."""

    def __init__(self, port):
        self.sock = socket.create_connection((fake.HOST, port), timeout=5)

    def send(self, data):
        self.sock.sendall(data)

    def read(self, count):
        data = b""
        while len(data) < count:
            chunk = self.sock.recv(count - len(data))
            if not chunk:
                raise AssertionError(f"closed after {len(data)} of {count} bytes")
            data += chunk
        return data

    def closed_by_broker(self):
        """Whether the broker closed its end (the next read gets EOF)."""
        try:
            return self.sock.recv(1) == b""
        except ConnectionResetError:
            return True

    def drop(self):
        """Closed at once, without DISCONNECT: a reset rather than a goodbye."""
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, b"\x01\x00\x00\x00\x00\x00\x00\x00")
        self.sock.close()

    def close(self):
        self.sock.close()


class BrokerTest(unittest.TestCase):
    def start(self, **kwargs):
        self.lines = []
        broker = fake.FakeBroker(log=self.lines.append, **kwargs).start()
        self.addCleanup(broker.stop)
        return broker

    def connected(self, broker, client_id="band3_pc", will=("band3/pc/status", "offline", True),
                  **kwargs):
        client = Client(broker.mqtt_port)
        self.addCleanup(client.close)
        client.send(connect_packet(client_id, will, **kwargs))
        return client

    def test_connect_with_a_will_is_accepted(self):
        broker = self.start()
        client = self.connected(broker)
        self.assertEqual(client.read(4), bytes([0x20, 0x02, 0x00, 0x00]))
        connect = broker.connects()[0]
        self.assertEqual(connect.client_id, "band3_pc")
        self.assertEqual(connect.will_topic, "band3/pc/status")
        self.assertEqual(connect.will_payload, b"offline")
        self.assertTrue(connect.will_retain)
        self.assertTrue(connect.clean_session)
        self.assertEqual(connect.keepalive_s, 60)
        self.assertIsNone(connect.username)
        self.assertEqual(connect.return_code, 0)

    def test_band3s_own_connect_bytes(self):
        # tests/mqtt_protocol_test.cpp's "CONNECT with a retained will and a user name"
        packet = (bytes([0x10, 52]) + string("MQTT") + bytes([0x04, 0xE6, 0x00, 0x3C])
                  + string("band3_pc") + string("band3/pc/status") + string("offline")
                  + string("u") + string("p"))
        broker = self.start(username="u", password="p")
        client = Client(broker.mqtt_port)
        self.addCleanup(client.close)
        # in two pieces, the length split from the rest
        client.send(packet[:1])
        time.sleep(0.05)
        client.send(packet[1:])
        self.assertEqual(client.read(4), bytes([0x20, 0x02, 0x00, 0x00]))
        connect = broker.connects()[0]
        self.assertEqual((connect.username, connect.password), ("u", "p"))
        self.assertEqual((connect.will_topic, connect.will_retain, connect.will_qos),
                         ("band3/pc/status", True, 0))
        # the password is never printed
        self.assertFalse(any("'p'" in line for line in self.lines), self.lines)
        self.assertTrue(any("with a password" in line for line in self.lines), self.lines)

    def test_publishes_are_recorded_and_retained_ones_kept(self):
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.send(publish_packet("homeassistant/sensor/band3_pc/song/config", '{"a":1}', True))
        client.send(publish_packet("band3/pc/song", "20th Century Boy"))
        client.send(publish_packet("band3/pc/song", "Bad Reputation"))
        client.send(publish_packet("band3/pc/status", "online", True))
        self.assertTrue(broker.wait_for(lambda: len(broker.publishes()) == 4, 5))
        first = broker.publishes()[0]
        self.assertEqual((first.topic, first.payload, first.retain, first.will),
                         ("homeassistant/sensor/band3_pc/song/config", b'{"a":1}', True, False))
        self.assertEqual([p.text for p in broker.publishes("band3/pc/song")],
                         ["20th Century Boy", "Bad Reputation"])
        self.assertEqual(broker.latest("band3/pc/song"), "Bad Reputation")
        self.assertIsNone(broker.latest("band3/pc/nothing"))
        self.assertEqual(broker.retained("band3/pc/status"), "online")
        self.assertIsNone(broker.retained("band3/pc/song"))
        self.assertEqual(set(broker.retained()),
                         {"homeassistant/sensor/band3_pc/song/config", "band3/pc/status"})
        # an empty retained payload deletes the topic
        client.send(publish_packet("homeassistant/sensor/band3_pc/song/config", "", True))
        self.assertTrue(broker.wait_for(
            lambda: broker.retained("homeassistant/sensor/band3_pc/song/config") is None, 5))

    def test_a_long_publish(self):
        # a remaining length of two bytes
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.send(publish_packet("band3/pc/big", "x" * 300, True))
        self.assertTrue(broker.wait_for(lambda: broker.retained("band3/pc/big") == "x" * 300, 5))

    def test_pingreq_gets_pingresp(self):
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.send(PINGREQ)
        self.assertEqual(client.read(2), bytes([0xD0, 0x00]))

    def test_dropped_connection_publishes_the_will(self):
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.send(publish_packet("band3/pc/status", "online", True))
        self.assertTrue(broker.wait_for(lambda: broker.retained("band3/pc/status") == "online", 5))
        client.drop()
        self.assertTrue(broker.wait_for(lambda: broker.retained("band3/pc/status") == "offline", 5))
        will = broker.publishes()[-1]
        self.assertEqual((will.topic, will.text, will.retain, will.will),
                         ("band3/pc/status", "offline", True, True))
        gone = broker.disconnects()[0]
        self.assertFalse(gone.clean)
        self.assertEqual(gone.client_id, "band3_pc")

    def test_closed_without_disconnect_publishes_the_will(self):
        # a plain close (FIN), as when the process exits
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.close()
        self.assertTrue(broker.wait_for(lambda: broker.retained("band3/pc/status") == "offline", 5))

    def test_disconnect_drops_the_will(self):
        broker = self.start()
        client = self.connected(broker)
        client.read(4)
        client.send(publish_packet("band3/pc/status", "online", True))
        client.send(DISCONNECT)
        self.assertTrue(broker.wait_for(lambda: broker.disconnects(), 5))
        self.assertTrue(broker.disconnects()[0].clean)
        client.close()
        time.sleep(0.2)
        self.assertEqual(broker.retained("band3/pc/status"), "online")
        self.assertFalse(any(p.will for p in broker.publishes()))

    def test_bad_credentials_get_code_4(self):
        broker = self.start(username="ha", password="secret")
        client = self.connected(broker, username="ha", password="wrong")
        self.assertEqual(client.read(4), bytes([0x20, 0x02, 0x00, 0x04]))
        self.assertTrue(client.closed_by_broker())
        self.assertEqual(broker.connects()[0].return_code, 4)
        # a refused client's will isn't published
        time.sleep(0.2)
        self.assertIsNone(broker.retained("band3/pc/status"))
        # and none at all is refused too
        other = self.connected(broker, client_id="other")
        self.assertEqual(other.read(4), bytes([0x20, 0x02, 0x00, 0x04]))
        right = self.connected(broker, client_id="right", username="ha", password="secret")
        self.assertEqual(right.read(4), bytes([0x20, 0x02, 0x00, 0x00]))

    def test_connack_code_is_configurable(self):
        broker = self.start(connack_code=5)
        client = self.connected(broker)
        self.assertEqual(client.read(4), bytes([0x20, 0x02, 0x00, 0x05]))

    def test_keepalive_running_out_publishes_the_will(self):
        broker = self.start()
        client = self.connected(broker, keepalive=1)
        client.read(4)
        # 1.5 s of silence
        self.assertTrue(broker.wait_for(lambda: broker.retained("band3/pc/status") == "offline", 5))
        self.assertIn("keepalive", broker.disconnects()[0].reason)

    def test_publish_before_connect_closes(self):
        broker = self.start()
        client = Client(broker.mqtt_port)
        self.addCleanup(client.close)
        client.send(publish_packet("band3/pc/song", "x"))
        self.assertTrue(client.closed_by_broker())
        self.assertEqual(broker.publishes(), [])

    def test_webhook_post_is_recorded(self):
        broker = self.start()
        body = json.dumps({"type": "state", "status": "playing"}).encode()
        request = urllib.request.Request(
            f"{broker.webhook_url_base}/api/webhook/rb3_event", data=body,
            headers={"Content-Type": "application/json"}, method="POST")
        with urllib.request.urlopen(request, timeout=5) as reply:
            self.assertEqual(reply.status, 200)
        hook = broker.webhooks()[0]
        self.assertEqual(hook.path, "/api/webhook/rb3_event")
        self.assertEqual(hook.content_type, "application/json")
        self.assertEqual(hook.json, {"type": "state", "status": "playing"})
        self.assertEqual(hook.body, body)

    def test_webhook_status_is_configurable(self):
        broker = self.start(webhook_status=500)
        request = urllib.request.Request(f"{broker.webhook_url_base}/api/webhook/x", data=b"no",
                                         headers={"Content-Type": "text/plain"}, method="POST")
        with self.assertRaises(urllib.error.HTTPError) as caught:
            urllib.request.urlopen(request, timeout=5)
        self.assertEqual(caught.exception.code, 500)
        self.assertIsNone(broker.webhooks()[0].json)

    def test_wait_for_times_out_falsy(self):
        broker = self.start()
        started = time.monotonic()
        self.assertFalse(broker.wait_for(lambda: broker.publishes(), 0.3))
        self.assertGreaterEqual(time.monotonic() - started, 0.3)

    def test_remaining_length(self):
        for length, expected in ((0, b"\x00"), (127, b"\x7f"), (128, b"\x80\x01"),
                                 (16383, b"\xff\x7f"), (16384, b"\x80\x80\x01")):
            self.assertEqual(fake.encode_remaining_length(length), expected)
        reader = fake.PacketReader()
        reader.add(b"\x30\xff\xff\xff\xff\x01")
        with self.assertRaises(fake.ProtocolError):
            reader.next()


if __name__ == "__main__":
    unittest.main()
