"""Tests for the mock router: python tools/test_port_mapping_mock.py

The packing against the golden packets band3's C++ codec is tested on, then a router
on ephemeral 127.0.0.1 ports asked the way band3 asks: PCP and NAT-PMP over UDP, UPnP's
description and SOAP actions over HTTP.
"""

import os
import socket
import unittest
import urllib.error
import urllib.request

import port_mapping_mock as mock

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GOLDEN = os.path.join(REPO, "tests", "golden", "port_mapping_packets.txt")

NONCE = bytes(range(1, 13))
PUBLIC = "203.0.113.5"


def read_golden():
    vectors = {}
    with open(GOLDEN, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith("#"):
                name, value = line.split()
                vectors[name] = bytes.fromhex(value)
    return vectors


class GoldenTest(unittest.TestCase):
    """The mock reads band3's requests as the golden file has them, and answers in its bytes."""

    @classmethod
    def setUpClass(cls):
        cls.golden = read_golden()

    def test_pcp_map(self):
        request = mock.decode_pcp_map(self.golden["pcp_map_request"])
        self.assertEqual(request, mock.PcpMap(3600, "192.168.1.20", NONCE, 17, 9103, 9103,
                                              "0.0.0.0"))
        self.assertEqual(mock.encode_pcp_map(NONCE, "192.168.1.20", 9103, 3600),
                         self.golden["pcp_map_request"])
        self.assertEqual(mock.encode_pcp_map_reply(request, PUBLIC, 42),
                         self.golden["pcp_map_reply"])
        self.assertEqual(mock.encode_pcp_map_reply(request, PUBLIC, 42, external_port=9104),
                         self.golden["pcp_map_reply_other_port"])
        self.assertIsNone(mock.decode_pcp_map(self.golden["pcp_map_request"][:-1]))

    def test_pcp_delete(self):
        request = mock.decode_pcp_map(self.golden["pcp_delete_request"])
        self.assertEqual(request.lifetime, 0)
        self.assertEqual(request.nonce, NONCE)
        self.assertEqual(mock.encode_pcp_map_reply(request, PUBLIC, 42),
                         self.golden["pcp_delete_reply"])

    def test_pcp_error(self):
        self.assertEqual(mock.encode_pcp_error(2, 42), self.golden["pcp_map_reply_not_authorized"])

    def test_unsupported_version(self):
        self.assertEqual(mock.encode_unsupported_version(mock.NATPMP_VERSION, mock.PCP_OP_MAP, 42),
                         self.golden["unsupported_version_natpmp"])
        self.assertEqual(mock.encode_unsupported_version(
            mock.PCP_VERSION, mock.NATPMP_OP_EXTERNAL_ADDRESS, 42),
            self.golden["unsupported_version_pcp"])

    def test_natpmp(self):
        self.assertEqual(mock.encode_natpmp_external_address(),
                         self.golden["natpmp_external_address_request"])
        self.assertEqual(mock.encode_natpmp_external_address_reply(PUBLIC, 42),
                         self.golden["natpmp_external_address_reply"])
        request = mock.decode_natpmp_map(self.golden["natpmp_map_request"])
        self.assertEqual(request, mock.NatPmpMap(9103, 9103, 3600))
        self.assertEqual(mock.encode_natpmp_map(9103, 9103, 3600), self.golden["natpmp_map_request"])
        self.assertEqual(mock.encode_natpmp_map_reply(request, 42), self.golden["natpmp_map_reply"])
        delete = mock.decode_natpmp_map(self.golden["natpmp_delete_request"])
        self.assertEqual(delete, mock.NatPmpMap(9103, 0, 0))
        self.assertEqual(mock.encode_natpmp_map_reply(delete, 42), self.golden["natpmp_delete_reply"])
        self.assertEqual(mock.encode_natpmp_map_reply(delete, 42, result=2),
                         self.golden["natpmp_map_reply_refused"])


class RouterTest(unittest.TestCase):
    def start(self, **options):
        self.lines = []
        router = mock.MockRouter(udp_port=0, log=self.lines.append, **options)
        router.start_http(0)
        router.serve_in_background()
        self.addCleanup(router.close)
        self.client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.client.settimeout(2)
        self.client.connect((mock.HOST, router.udp_port))
        self.addCleanup(self.client.close)
        return router

    def ask(self, data):
        self.client.send(data)
        return self.client.recv(1500)

    def logged(self, text):
        return any(text in line for line in self.lines)


class UdpTest(RouterTest):
    def test_listens_on_this_pc_only(self):
        router = self.start()
        self.assertEqual(router.udp.getsockname()[0], "127.0.0.1")
        self.assertEqual(router.http.server_address[0], "127.0.0.1")

    def test_pcp_maps_and_deletes(self):
        self.start(mode="pcp")
        reply = self.ask(mock.encode_pcp_map(NONCE, "127.0.0.1", 9103, 3600))
        self.assertEqual(len(reply), 60)
        self.assertEqual(reply[:4], bytes([2, 0x81, 0, 0]))
        self.assertEqual(reply[24:36], NONCE)
        self.assertEqual(reply[42:44], (9103).to_bytes(2, "big"))
        self.assertEqual(socket.inet_ntoa(reply[56:60]), PUBLIC)
        self.assertTrue(self.logged("pcp: map UDP 9103 lifetime 3600"))
        self.ask(mock.encode_pcp_map(NONCE, "127.0.0.1", 9103, 0))
        self.assertTrue(self.logged("pcp: delete UDP 9103"))

    def test_pcp_can_assign_another_port(self):
        self.start(mode="pcp", assign_port=9104)
        reply = self.ask(mock.encode_pcp_map(NONCE, "127.0.0.1", 9103, 3600))
        self.assertEqual(reply[42:44], (9104).to_bytes(2, "big"))

    def test_natpmp_mode_turns_pcp_away_and_speaks_natpmp(self):
        self.start(mode="natpmp")
        reply = self.ask(mock.encode_pcp_map(NONCE, "127.0.0.1", 9103, 3600))
        # NAT-PMP's "unsupported version", in its own 8 bytes
        self.assertEqual(len(reply), 8)
        self.assertEqual(reply[:4], bytes([0, 0x81, 0, 1]))
        reply = self.ask(mock.encode_natpmp_external_address())
        self.assertEqual(reply[:4], bytes([0, 0x80, 0, 0]))
        self.assertEqual(socket.inet_ntoa(reply[8:12]), PUBLIC)
        reply = self.ask(mock.encode_natpmp_map(9103, 9103, 3600))
        self.assertEqual(reply[:4], bytes([0, 0x81, 0, 0]))
        self.assertEqual(reply[10:12], (9103).to_bytes(2, "big"))
        self.ask(mock.encode_natpmp_map(9103, 0, 0))
        self.assertTrue(self.logged("natpmp: external address"))
        self.assertTrue(self.logged("natpmp: map UDP 9103 lifetime 3600"))
        self.assertTrue(self.logged("natpmp: delete UDP 9103"))

    def test_pcp_mode_turns_natpmp_away(self):
        self.start(mode="pcp")
        reply = self.ask(mock.encode_natpmp_external_address())
        self.assertEqual(reply[0], mock.PCP_VERSION)
        self.assertEqual(reply[3], 1)

    def test_silent_answers_nothing_but_logs_it(self):
        self.start(mode="silent")
        self.client.settimeout(0.5)
        self.client.send(mock.encode_pcp_map(NONCE, "127.0.0.1", 9103, 3600))
        with self.assertRaises(socket.timeout):
            self.client.recv(1500)
        self.assertTrue(self.logged("udp: ignored 60 bytes"))


class UpnpTest(RouterTest):
    def soap(self, router, action, **args):
        body = "".join(f"<{name}>{value}</{name}>" for name, value in args.items())
        request = urllib.request.Request(
            f"http://127.0.0.1:{router.http_port}/ctl", data=body.encode(),
            headers={"SOAPAction": f'"{mock.SERVICE}#{action}"'})
        try:
            with urllib.request.urlopen(request, timeout=5) as response:
                return response.status, response.read().decode()
        except urllib.error.HTTPError as e:
            return e.code, e.read().decode()

    def add(self, router, lease):
        return self.soap(router, "AddPortMapping", NewRemoteHost="", NewExternalPort=9103,
                         NewProtocol="UDP", NewInternalPort=9103, NewInternalClient="127.0.0.1",
                         NewEnabled=1, NewPortMappingDescription="band3",
                         NewLeaseDuration=lease)

    def test_description_names_the_control_url(self):
        router = self.start()
        with urllib.request.urlopen(f"http://127.0.0.1:{router.http_port}/desc.xml",
                                    timeout=5) as response:
            description = response.read().decode()
        self.assertIn(mock.SERVICE, description)
        self.assertIn("<controlURL>/ctl</controlURL>", description)

    def test_maps_reports_and_deletes(self):
        router = self.start()
        status, body = self.soap(router, "GetExternalIPAddress")
        self.assertEqual(status, 200)
        self.assertIn(f"<NewExternalIPAddress>{PUBLIC}</NewExternalIPAddress>", body)
        self.assertEqual(self.add(router, 3600)[0], 200)
        status, body = self.soap(router, "GetSpecificPortMappingEntry", NewRemoteHost="",
                                 NewExternalPort=9103, NewProtocol="UDP")
        self.assertEqual(status, 200)
        self.assertIn("<NewLeaseDuration>3600</NewLeaseDuration>", body)
        self.assertEqual(self.soap(router, "DeletePortMapping", NewRemoteHost="",
                                   NewExternalPort=9103, NewProtocol="UDP")[0], 200)
        status, body = self.soap(router, "DeletePortMapping", NewRemoteHost="",
                                 NewExternalPort=9103, NewProtocol="UDP")
        self.assertEqual(status, 500)
        self.assertIn("<errorCode>714</errorCode>", body)
        self.assertTrue(self.logged("upnp: map UDP 9103 lifetime 3600"))
        self.assertTrue(self.logged("upnp: delete UDP 9103"))

    def test_permanent_only_router_takes_lease_zero(self):
        router = self.start(upnp_error=725)
        status, body = self.add(router, 3600)
        self.assertEqual(status, 500)
        self.assertIn("<errorCode>725</errorCode>", body)
        self.assertEqual(self.add(router, 0)[0], 200)
        self.assertTrue(self.logged("upnp: map UDP 9103 lifetime 3600 for 127.0.0.1:9103 -> error 725"))
        self.assertTrue(self.logged("upnp: map UDP 9103 lifetime 0"))

    def test_other_errors_refuse_every_mapping(self):
        router = self.start(upnp_error=718)
        self.assertEqual(self.add(router, 0)[0], 500)


if __name__ == "__main__":
    unittest.main()
