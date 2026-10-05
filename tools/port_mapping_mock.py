"""port_mapping_mock: a stand-in router for band3's port mapping, on 127.0.0.1 only.

band3 asks the router to forward liveless_port to its PC the way RB3Enhanced does: PCP
(RFC 6887) first, NAT-PMP (RFC 6886) if the router doesn't speak PCP, then UPnP. This
answers all three on this PC, so band3 is tested without ever asking a real router:

  python tools/port_mapping_mock.py --udp-port 5451 --http-port 8089 --mode pcp

and band3 started with both overrides pointing here (the test harness only maps a port
with them, never through the real router):

  --liveless_gateway=127.0.0.1:5451 --liveless_upnp_url=http://127.0.0.1:8089/desc.xml

--mode pcp answers PCP; natpmp answers PCP with "unsupported version" and speaks
NAT-PMP; silent never answers UDP, so band3 goes on to UPnP after 3.5 s. Every mapping
gets --external as the public address (203.0.113.5, an address kept for documentation).
--upnp-error 725 refuses UPnP mappings with a lease (OnlyPermanentLeasesSupported), so
only a permanent one (lease 0) works; 718 (ConflictInMappingEntry) has an old band3
mapping (one a band3 that crashed didn't delete) hold each port, to --stale-client
(127.0.0.1, band3's own PC, unless given), and refuses mappings of a port until a
DeletePortMapping clears it; another code refuses every UPnP mapping.
--assign-port maps PCP and NAT-PMP requests to that port outside instead of the one asked.

It logs one line per request: `pcp: map UDP 9103 lifetime 3600`, `natpmp: delete UDP
9103`, `upnp: map UDP 9103 lifetime 0`... and in silent mode `udp: ignored` for each
datagram, so a log with no request lines means band3 sent nothing.

The packing functions are kept apart from the server for
tools/test_port_mapping_mock.py, which checks them against
tests/golden/port_mapping_packets.txt, the bytes band3's C++ codec is held to.

Standard library only.
"""

import argparse
import collections
import datetime
import http.server
import re
import select
import socket
import struct
import sys
import threading
import time

HOST = "127.0.0.1"
GATEWAY_PORT = 5351
NATPMP_VERSION, PCP_VERSION = 0, 2
PCP_OP_MAP = 1
NATPMP_OP_EXTERNAL_ADDRESS, NATPMP_OP_MAP_UDP = 0, 1
REPLY = 0x80
UNSUPPORTED_VERSION = 1
PROTOCOL_UDP = 17
SERVICE = "urn:schemas-upnp-org:service:WANIPConnection:1"

_PCP_MAP = struct.Struct(">BBxxI16s12sBxxxHH16s")
_PCP_MAP_REPLY = struct.Struct(">BBxBII12x12sBxxxHH16s")
_NATPMP_MAP = struct.Struct(">BBxxHHI")
_NATPMP_ADDRESS_REPLY = struct.Struct(">BBHI4s")
_NATPMP_MAP_REPLY = struct.Struct(">BBHIHHI")
_UNSUPPORTED_NATPMP = struct.Struct(">BBHI")
_UNSUPPORTED_PCP = struct.Struct(">BBxBII12x")

PcpMap = collections.namedtuple(
    "PcpMap", "lifetime client nonce protocol internal_port external_port suggested")
NatPmpMap = collections.namedtuple("NatPmpMap", "internal_port external_port lifetime")


def _mapped(ip_text):
    """An IPv4 address as PCP carries it, IPv4-mapped IPv6."""
    return b"\0" * 10 + b"\xff\xff" + socket.inet_aton(ip_text)


def _unmapped(data):
    return socket.inet_ntoa(data[12:16])


# band3's requests

def encode_pcp_map(nonce, client, port, lifetime):
    return _PCP_MAP.pack(PCP_VERSION, PCP_OP_MAP, lifetime, _mapped(client), nonce,
                         PROTOCOL_UDP, port, port, _mapped("0.0.0.0"))


def decode_pcp_map(data):
    if len(data) < _PCP_MAP.size or data[0] != PCP_VERSION or data[1] != PCP_OP_MAP:
        return None
    _, _, lifetime, client, nonce, protocol, internal, external, suggested = \
        _PCP_MAP.unpack_from(data)
    return PcpMap(lifetime, _unmapped(client), nonce, protocol, internal, external,
                  _unmapped(suggested))


def encode_natpmp_external_address():
    return bytes([NATPMP_VERSION, NATPMP_OP_EXTERNAL_ADDRESS])


def encode_natpmp_map(internal_port, external_port, lifetime):
    return _NATPMP_MAP.pack(NATPMP_VERSION, NATPMP_OP_MAP_UDP, internal_port, external_port,
                            lifetime)


def decode_natpmp_map(data):
    if len(data) < _NATPMP_MAP.size or data[0] != NATPMP_VERSION or data[1] != NATPMP_OP_MAP_UDP:
        return None
    _, _, internal, external, lifetime = _NATPMP_MAP.unpack_from(data)
    return NatPmpMap(internal, external, lifetime)


# the router's replies

def encode_pcp_map_reply(request, external_ip, epoch, result=0, external_port=None):
    port = request.external_port if external_port is None else external_port
    return _PCP_MAP_REPLY.pack(PCP_VERSION, REPLY | PCP_OP_MAP, result, request.lifetime, epoch,
                               request.nonce, request.protocol, request.internal_port, port,
                               _mapped(external_ip))


def encode_pcp_error(result, epoch):
    """A PCP MAP failure without the MAP part."""
    return _UNSUPPORTED_PCP.pack(PCP_VERSION, REPLY | PCP_OP_MAP, result, 0, epoch)


def encode_natpmp_external_address_reply(external_ip, epoch, result=0):
    return _NATPMP_ADDRESS_REPLY.pack(NATPMP_VERSION, REPLY | NATPMP_OP_EXTERNAL_ADDRESS, result,
                                      epoch, socket.inet_aton(external_ip))


def encode_natpmp_map_reply(request, epoch, result=0, external_port=None):
    port = request.external_port if external_port is None else external_port
    return _NATPMP_MAP_REPLY.pack(NATPMP_VERSION, REPLY | NATPMP_OP_MAP_UDP, result, epoch,
                                  request.internal_port, port, request.lifetime)


def encode_unsupported_version(version, opcode, epoch):
    """"Unsupported version" in the router's own protocol: a NAT-PMP router's (version 0)
    answer to PCP, or a PCP-only router's (version 2) to NAT-PMP."""
    if version == NATPMP_VERSION:
        return _UNSUPPORTED_NATPMP.pack(NATPMP_VERSION, REPLY | opcode, UNSUPPORTED_VERSION, epoch)
    return _UNSUPPORTED_PCP.pack(PCP_VERSION, REPLY | opcode, UNSUPPORTED_VERSION, 0, epoch)


# UPnP

DESCRIPTION = """<?xml version="1.0"?>
<root xmlns="urn:schemas-upnp-org:device-1-0">
<specVersion><major>1</major><minor>0</minor></specVersion>
<device>
<deviceType>urn:schemas-upnp-org:device:InternetGatewayDevice:1</deviceType>
<friendlyName>band3 mock router</friendlyName>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANDevice:1</deviceType>
<deviceList><device>
<deviceType>urn:schemas-upnp-org:device:WANConnectionDevice:1</deviceType>
<serviceList><service>
<serviceType>%s</serviceType>
<serviceId>urn:upnp-org:serviceId:WANIPConn1</serviceId>
<controlURL>/ctl</controlURL>
<eventSubURL>/evt</eventSubURL>
<SCPDURL>/wanip.xml</SCPDURL>
</service></serviceList>
</device></deviceList>
</device></deviceList>
</device>
</root>
""" % SERVICE

UPNP_ERRORS = {
    402: "Invalid Args",
    714: "NoSuchEntryInArray",
    718: "ConflictInMappingEntry",
    725: "OnlyPermanentLeasesSupported",
}


def soap_response(action, values=()):
    body = "".join(f"<{name}>{value}</{name}>" for name, value in values)
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
            f'<u:{action}Response xmlns:u="{SERVICE}">{body}</u:{action}Response>'
            '</s:Body></s:Envelope>\r\n')


def soap_fault(code):
    return ('<?xml version="1.0"?>\r\n<s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
            's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body><s:Fault>'
            '<faultcode>s:Client</faultcode><faultstring>UPnPError</faultstring><detail>'
            '<UPnPError xmlns="urn:schemas-upnp-org:control-1-0">'
            f'<errorCode>{code}</errorCode><errorDescription>{UPNP_ERRORS.get(code, "Error")}'
            '</errorDescription></UPnPError></detail></s:Fault></s:Body></s:Envelope>\r\n')


def soap_args(body):
    """A SOAP request's arguments by name (miniupnpc sends them unprefixed)."""
    return {m.group(1): m.group(2) for m in re.finditer(r"<(New\w+)>([^<]*)</\1>", body)}


class MockRouter:
    """PCP and NAT-PMP on udp_port; UPnP once start_http() has its port. Each serves on a
    thread of its own (serve_in_background) until close()."""

    def __init__(self, udp_port=GATEWAY_PORT, mode="pcp", external="203.0.113.5",
                 upnp_error=0, assign_port=0, stale_client=HOST, log=print):
        self.mode = mode
        self.external = external
        self.upnp_error = upnp_error
        self.assign_port = assign_port
        self.stale_client = stale_client
        self.log = log
        self.start = time.monotonic()
        self.stopping = False
        self.lock = threading.Lock()
        # (protocol, port) -> lifetime, what UPnP has mapped
        self.upnp_mappings = {}
        # with upnp_error 718, the (protocol, port)s whose old mapping is deleted
        self.cleared = set()
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.bind((HOST, udp_port))
        self.udp_port = self.udp.getsockname()[1]
        self.http = None
        self.http_port = 0
        self.threads = []

    def epoch(self):
        return int(time.monotonic() - self.start)

    # UDP

    def serve_udp(self):
        while not self.stopping:
            ready, _, _ = select.select([self.udp], [], [], 0.1)
            if not ready:
                continue
            try:
                data, peer = self.udp.recvfrom(1500)
            except OSError:
                continue
            reply = self.answer_udp(data, peer)
            if reply is not None:
                self.udp.sendto(reply, peer)

    def answer_udp(self, data, peer):
        if self.mode == "silent":
            self.log(f"udp: ignored {len(data)} bytes from {peer[0]}:{peer[1]} (silent)")
            return None
        if len(data) < 2:
            self.log(f"udp: {len(data)} bytes, too short")
            return None
        version, opcode = data[0], data[1]
        if version == PCP_VERSION:
            if self.mode != "pcp":
                self.log(f"pcp: unsupported version, answered as NAT-PMP ({self.mode} mode)")
                return encode_unsupported_version(NATPMP_VERSION, opcode, self.epoch())
            request = decode_pcp_map(data)
            if request is None:
                self.log(f"pcp: opcode {opcode}, {len(data)} bytes: not a MAP")
                return encode_pcp_error(3, self.epoch())  # MALFORMED_REQUEST
            if request.lifetime == 0:
                self.log(f"pcp: delete UDP {request.internal_port} (client {request.client})")
            else:
                self.log(f"pcp: map UDP {request.internal_port} lifetime {request.lifetime} "
                         f"(client {request.client}, nonce {request.nonce.hex()})")
            return encode_pcp_map_reply(request, self.external, self.epoch(),
                                        external_port=self.assign_port or None)
        if version == NATPMP_VERSION:
            if self.mode != "natpmp":
                self.log(f"natpmp: unsupported version, answered as PCP ({self.mode} mode)")
                return encode_unsupported_version(PCP_VERSION, opcode, self.epoch())
            if opcode == NATPMP_OP_EXTERNAL_ADDRESS:
                self.log("natpmp: external address")
                return encode_natpmp_external_address_reply(self.external, self.epoch())
            request = decode_natpmp_map(data)
            if request is None:
                self.log(f"natpmp: opcode {opcode}, {len(data)} bytes: not a UDP map")
                return None
            if request.lifetime == 0:
                self.log(f"natpmp: delete UDP {request.internal_port}")
                return encode_natpmp_map_reply(request, self.epoch())
            self.log(f"natpmp: map UDP {request.internal_port} lifetime {request.lifetime}")
            return encode_natpmp_map_reply(request, self.epoch(),
                                           external_port=self.assign_port or None)
        self.log(f"udp: version {version}, {len(data)} bytes: neither PCP nor NAT-PMP")
        return None

    # UPnP

    def held_by_stale(self, key):
        """Whether the old mapping 718 has holds key still (call with the lock held)."""
        return self.upnp_error == 718 and key not in self.cleared

    def answer_soap(self, action, args):
        """(HTTP status, body) for a SOAP action."""
        if action == "GetExternalIPAddress":
            self.log("upnp: GetExternalIPAddress")
            return 200, soap_response(action, [("NewExternalIPAddress", self.external)])
        protocol = args.get("NewProtocol", "")
        port = args.get("NewExternalPort", "")
        key = (protocol, port)
        if action == "AddPortMapping":
            lease = args.get("NewLeaseDuration", "0")
            with self.lock:
                if self.upnp_error == 718:
                    refused = self.held_by_stale(key)
                else:
                    refused = self.upnp_error and (self.upnp_error != 725 or lease != "0")
            self.log(f"upnp: map {protocol} {port} lifetime {lease} for "
                     f"{args.get('NewInternalClient', '')}:{args.get('NewInternalPort', '')}"
                     + (f" -> error {self.upnp_error}" if refused else ""))
            if refused:
                return 500, soap_fault(self.upnp_error)
            with self.lock:
                self.upnp_mappings[key] = (args.get("NewInternalClient", ""), lease)
            return 200, soap_response(action)
        if action == "DeletePortMapping":
            with self.lock:
                stale = self.held_by_stale(key)
                found = stale or self.upnp_mappings.pop(key, None) is not None
                self.cleared.add(key)
            self.log(f"upnp: delete {protocol} {port}" + (" (the old mapping)" if stale else "")
                     + ("" if found else " -> error 714"))
            return (200, soap_response(action)) if found else (500, soap_fault(714))
        if action == "GetSpecificPortMappingEntry":
            with self.lock:
                if self.held_by_stale(key):
                    entry = (self.stale_client, "0")
                else:
                    entry = self.upnp_mappings.get(key)
            self.log(f"upnp: GetSpecificPortMappingEntry {protocol} {port}"
                     + (f" -> {entry[0]}" if entry is not None else " -> error 714"))
            if entry is None:
                return 500, soap_fault(714)
            client, lease = entry
            return 200, soap_response(action, [("NewInternalPort", port),
                                               ("NewInternalClient", client),
                                               ("NewEnabled", "1"),
                                               ("NewPortMappingDescription", "band3"),
                                               ("NewLeaseDuration", lease)])
        self.log(f"upnp: {action}: not an action the mock knows")
        return 500, soap_fault(402)

    def start_http(self, port):
        router = self

        class Handler(http.server.BaseHTTPRequestHandler):
            protocol_version = "HTTP/1.1"

            def send(self, status, body, content_type='text/xml; charset="utf-8"'):
                data = body.encode()
                self.send_response(status)
                self.send_header("Content-Type", content_type)
                self.send_header("Content-Length", str(len(data)))
                self.send_header("Connection", "close")
                self.end_headers()
                self.wfile.write(data)
                self.close_connection = True

            def do_GET(self):
                router.log(f"http: GET {self.path}")
                if self.path == "/desc.xml":
                    self.send(200, DESCRIPTION)
                else:
                    self.send(404, "not found", "text/plain")

            def do_POST(self):
                length = int(self.headers.get("Content-Length", "0"))
                body = self.rfile.read(length).decode(errors="replace")
                action = self.headers.get("SOAPAction", "").strip('"').rpartition("#")[2]
                if self.path != "/ctl":
                    router.log(f"http: POST {self.path}: no such control URL")
                    self.send(404, "not found", "text/plain")
                    return
                self.send(*router.answer_soap(action, soap_args(body)))

            def log_message(self, *args):
                pass

        self.http = http.server.ThreadingHTTPServer((HOST, port), Handler)
        self.http.daemon_threads = True
        self.http_port = self.http.server_address[1]

    def serve_in_background(self):
        """Serves UDP (and HTTP, if started) on threads of their own."""
        udp = threading.Thread(target=self.serve_udp, daemon=True)
        udp.start()
        self.threads.append(udp)
        if self.http:
            web = threading.Thread(target=self.http.serve_forever, kwargs={"poll_interval": 0.1},
                                   daemon=True)
            web.start()
            self.threads.append(web)

    def close(self):
        self.stopping = True
        if self.http:
            self.http.shutdown()
            self.http.server_close()
        for thread in self.threads:
            thread.join(timeout=2)
        self.udp.close()


def main(argv):
    parser = argparse.ArgumentParser(
        description="A mock router answering PCP, NAT-PMP and UPnP on 127.0.0.1, for testing band3.")
    parser.add_argument("--udp-port", type=int, default=GATEWAY_PORT,
                        help="PCP and NAT-PMP's port (a router's is 5351)")
    parser.add_argument("--http-port", type=int, default=0,
                        help="UPnP's description (/desc.xml) and control URL (/ctl); 0: no UPnP")
    parser.add_argument("--mode", choices=["pcp", "natpmp", "silent"], default="pcp",
                        help="pcp answers PCP; natpmp only NAT-PMP; silent no UDP at all")
    parser.add_argument("--external", default="203.0.113.5",
                        help="the public IPv4 address every mapping gets")
    parser.add_argument("--upnp-error", type=int, default=0,
                        help="refuse UPnP mappings with this error (725: only those with a "
                             "lease; 718: until an old mapping of the port is deleted)")
    parser.add_argument("--stale-client", default=HOST,
                        help="with --upnp-error 718, the PC the old mapping forwards to")
    parser.add_argument("--assign-port", type=int, default=0,
                        help="map PCP and NAT-PMP requests to this port outside")
    args = parser.parse_args(argv)

    def log(line):
        stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
        print(f"{stamp} {line}", flush=True)

    try:
        router = MockRouter(udp_port=args.udp_port, mode=args.mode, external=args.external,
                            upnp_error=args.upnp_error, assign_port=args.assign_port,
                            stale_client=args.stale_client, log=log)
        if args.http_port:
            router.start_http(args.http_port)
    except OSError as e:
        print(f"can't listen on {HOST}: {e}", file=sys.stderr)
        return 1
    router.serve_in_background()
    log(f"listening on {HOST}:{router.udp_port} (udp, {args.mode})"
        + (f", http://{HOST}:{router.http_port}/desc.xml" if router.http else "")
        + f", public address {args.external}")
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
    finally:
        router.close()
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
