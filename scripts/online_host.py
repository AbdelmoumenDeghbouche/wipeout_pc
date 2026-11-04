"""Hosting and joining online games without typing addresses (docs/online-multiplayer.md).

Used by the launcher's Online page. The host's router is asked to open the port (UPnP), a STUN
server tells the public address, and both go into a short join code. No service of this project is
involved: the code *is* the host's address, packed.

Standard library only, no Tk: the launcher runs this on a helper thread.
"""
import atexit
import ipaddress
import re
import secrets
import socket
import struct
import threading
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET
import zlib

DEFAULT_PORT = 7788
PORT_TRIES = 10
LEASE_SECONDS = 3600          # a crash leaves the router clean within the hour
MAPPING_NAME = 'WipEout Omega online'
STUN_SERVERS = (('stun.cloudflare.com', 3478), ('stun.l.google.com', 19302))

# What can be said about being reachable from the internet.
OPEN = 'open'                 # the router opened the port, and its address is the public one
MANUAL = 'manual'             # no answer from the router, or it refused: forward the port by hand
SHARED = 'shared'             # the internet provider or a second router is in the way (CGNAT)
LAN_ONLY = 'lan'              # no public address found: only this network can join


# ---- join codes ----
# Crockford base32: no I, L, O, U, and those letters read as the digits they look like. The first
# byte holds the kind of LAN address (2 bits), whether a port follows (1 bit) and a checksum (5).
ALPHABET = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
LAN_NONE, LAN_192_168, LAN_FULL = 0, 1, 2


def _checksum(flags, body):
    return zlib.crc32(bytes([flags]) + body) & 0x1F


def encode_code(public_ip, port=DEFAULT_PORT, lan_ip=None):
    """The join code of a host, for example 7ZK2-MQ4T-X81A."""
    body = socket.inet_aton(public_ip)
    flags = 0
    if port != DEFAULT_PORT:
        flags |= 4
        body += struct.pack('>H', port)
    if lan_ip and lan_ip != public_ip:
        packed = socket.inet_aton(lan_ip)
        if packed[:2] == b'\xc0\xa8':
            flags |= LAN_192_168
            body += packed[2:]
        else:
            flags |= LAN_FULL
            body += packed
    data = bytes([flags | _checksum(flags, body) << 3]) + body
    bits = len(data) * 8
    pad = -bits % 5
    value = int.from_bytes(data, 'big') << pad
    chars = (bits + pad) // 5
    text = ''.join(ALPHABET[value >> 5 * (chars - 1 - i) & 31] for i in range(chars))
    return '-'.join(text[i:i + 4] for i in range(0, len(text), 4))


def decode_code(code):
    """(public_ip, port, lan_ip or None) of a join code; ValueError when it is not a valid one."""
    text = re.sub(r'[\s-]', '', code).upper().translate(str.maketrans('OIL', '011'))
    if not text or any(c not in ALPHABET for c in text):
        raise ValueError('not a join code')
    value = 0
    for c in text:
        value = value << 5 | ALPHABET.index(c)
    size = len(text) * 5 // 8
    pad = len(text) * 5 - size * 8
    if size < 5 or pad >= 5 or value & ((1 << pad) - 1):
        raise ValueError('not a join code')
    data = (value >> pad).to_bytes(size, 'big')
    flags, body = data[0] & 7, data[1:]
    if data[0] >> 3 != _checksum(flags, body):
        raise ValueError('mistyped join code')
    lan_kind = flags & 3
    expected = 4 + (2 if flags & 4 else 0) + {LAN_NONE: 0, LAN_192_168: 2, LAN_FULL: 4}.get(lan_kind, -99)
    if len(body) != expected:
        raise ValueError('not a join code')
    public_ip = socket.inet_ntoa(body[:4])
    rest = body[4:]
    port = DEFAULT_PORT
    if flags & 4:
        port = struct.unpack('>H', rest[:2])[0]
        rest = rest[2:]
    lan_ip = None
    if lan_kind == LAN_192_168:
        lan_ip = socket.inet_ntoa(b'\xc0\xa8' + rest)
    elif lan_kind == LAN_FULL:
        lan_ip = socket.inet_ntoa(rest)
    return public_ip, port, lan_ip


def join_candidates(text):
    """The host:port addresses to try for what a player typed: a join code, or a plain address
    (a dedicated server). A code's LAN address comes first: a router often cannot reach its own
    public address from inside. ValueError when it is neither."""
    text = text.strip()
    if '.' in text or ':' in text:
        host, _, port = text.partition(':')
        if not host or (port and not (port.isdigit() and 0 < int(port) < 65536)):
            raise ValueError('not an address')
        return [f'{host}:{port or DEFAULT_PORT}']
    public_ip, port, lan_ip = decode_code(text)
    return ([f'{lan_ip}:{port}'] if lan_ip else []) + [f'{public_ip}:{port}']


def reachable_address(candidates, timeout=2.5):
    """The first of the addresses that accepts a connection; None when none does."""
    results = {}

    def probe(address):
        host, _, port = address.rpartition(':')
        try:
            with socket.create_connection((host, int(port)), timeout=timeout):
                results[address] = True
        except OSError:
            results[address] = False

    threads = [threading.Thread(target=probe, args=(a,), daemon=True) for a in candidates]
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout + 0.5)
    return next((a for a in candidates if results.get(a)), None)


# ---- the online name ----
def online_name(player_name, machine=None):
    """What other players see, for the launcher's Player name: as src/runtime_np.c sanitizes it
    (a PSN id: 3 to 16 of letters, digits, - and _). Empty: Player- and a code of this PC's name,
    so that two PCs left at the default do not take each other's place."""
    name = ''
    for c in player_name.strip():
        if len(name) == 16:
            break
        if c.isascii() and (c.isalnum() or c in '-_'):
            name += c
        elif c == ' ' and name:
            name += '_'
    if not name:
        machine = socket.gethostname() if machine is None else machine
        return f'Player-{zlib.crc32(machine.encode()) & 0xFFFF:04x}'
    return name + 'P12'[len(name):] if len(name) < 3 else name


# ---- addresses ----
def local_address(towards='8.8.8.8'):
    """This PC's address on the way to `towards`. UDP connect sends nothing. None when offline."""
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        try:
            s.connect((towards, 53))
            return s.getsockname()[0]
        except OSError:
            return None


def is_public(ip):
    """A public internet address: not private, and not the 100.64/10 of carrier-grade NAT."""
    try:
        return ipaddress.ip_address(ip).is_global
    except ValueError:
        return False


def stun_public_ip(servers=STUN_SERVERS, timeout=1.5):
    """The address the internet sees, from a STUN binding request (RFC 5389); None when no server
    answers. Sends nothing but the request: a server learns the address and nothing else."""
    for host, port in servers:
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.settimeout(timeout)
                transaction = secrets.token_bytes(12)
                s.sendto(struct.pack('>HHI', 1, 0, 0x2112A442) + transaction, (host, port))
                data, _ = s.recvfrom(1024)
        except OSError:
            continue
        ip = parse_stun_response(data, transaction)
        if ip:
            return ip
    return None


def parse_stun_response(data, transaction):
    if len(data) < 20 or data[:2] != b'\x01\x01' or data[8:20] != transaction:
        return None
    position, mapped = 20, None
    while position + 4 <= len(data):
        kind, size = struct.unpack_from('>HH', data, position)
        value = data[position + 4:position + 4 + size]
        if len(value) == 8 and value[1] == 1:  # IPv4
            if kind == 0x0020:  # XOR-MAPPED-ADDRESS
                return socket.inet_ntoa(bytes(a ^ b for a, b in zip(value[4:], b'\x21\x12\xa4\x42')))
            if kind == 0x0001:  # MAPPED-ADDRESS
                mapped = socket.inet_ntoa(value[4:])
        position += 4 + size + (-size % 4)
    return mapped


# ---- UPnP ----
SEARCH_TARGETS = ('urn:schemas-upnp-org:device:InternetGatewayDevice:2',
                  'urn:schemas-upnp-org:device:InternetGatewayDevice:1',
                  'urn:schemas-upnp-org:service:WANIPConnection:1',
                  'urn:schemas-upnp-org:service:WANPPPConnection:1')
WAN_SERVICES = ('WANIPConnection', 'WANPPPConnection')
CONFLICT_ERRORS = {718, 724, 728}  # the port is taken, or must match another
ONLY_PERMANENT = 725


class UpnpError(Exception):
    def __init__(self, code, text=''):
        super().__init__(f'UPnP error {code}: {text}')
        self.code = code


class Gateway:
    """A router's port mapping service."""

    def __init__(self, control_url, service):
        self.control_url = control_url
        self.service = service

    def call(self, action, **arguments):
        body = ''.join(f'<{k}>{v}</{k}>' for k, v in arguments.items())
        envelope = ('<?xml version="1.0"?><s:Envelope xmlns:s="http://schemas.xmlsoap.org/soap/envelope/" '
                    's:encodingStyle="http://schemas.xmlsoap.org/soap/encoding/"><s:Body>'
                    f'<u:{action} xmlns:u="{self.service}">{body}</u:{action}></s:Body></s:Envelope>')
        request = urllib.request.Request(self.control_url, envelope.encode(), {
            'Content-Type': 'text/xml; charset="utf-8"', 'SOAPAction': f'"{self.service}#{action}"'})
        try:
            with _opener.open(request, timeout=4) as reply:
                return reply.read(65536).decode('utf-8', 'replace')
        except urllib.error.HTTPError as error:
            text = error.read(65536).decode('utf-8', 'replace')
            code = re.search(r'<errorCode>\s*(\d+)', text)
            detail = re.search(r'<errorDescription>([^<]*)', text)
            raise UpnpError(int(code.group(1)) if code else error.code, detail.group(1) if detail else '') from None
        except OSError as error:
            raise UpnpError(0, str(error)) from None

    def external_ip(self):
        found = re.search(r'<NewExternalIPAddress>\s*([0-9.]+)', self.call('GetExternalIPAddress'))
        return found.group(1) if found else None

    def add(self, protocol, port, client, lease):
        self.call('AddPortMapping', NewRemoteHost='', NewExternalPort=port, NewProtocol=protocol,
                  NewInternalPort=port, NewInternalClient=client, NewEnabled=1,
                  NewPortMappingDescription=MAPPING_NAME, NewLeaseDuration=lease)

    def delete(self, protocol, port):
        self.call('DeletePortMapping', NewRemoteHost='', NewExternalPort=port, NewProtocol=protocol)


# Router addresses must not go through a system proxy.
_opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def gateways_from_description(xml_text, location):
    """The port mapping services of a device description, IPv4 connections first."""
    found = []
    for element in ET.fromstring(xml_text).iter():
        if not element.tag.endswith('service'):
            continue
        fields = {child.tag.rpartition('}')[2]: (child.text or '').strip() for child in element}
        service, control = fields.get('serviceType', ''), fields.get('controlURL', '')
        if control and any(f':{name}:' in service for name in WAN_SERVICES):
            found.append(Gateway(urllib.parse.urljoin(location, control), service))
    found.sort(key=lambda g: 'WANPPP' in g.service)
    return found


def discover_gateways(lan_ip, timeout=2.0):
    """The routers that answer on the network of `lan_ip`."""
    locations = {}
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.IPPROTO_IP, socket.IP_MULTICAST_TTL, 2)
        s.settimeout(timeout)
        s.bind((lan_ip, 0))
        for target in SEARCH_TARGETS:
            s.sendto(('M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\nMAN: "ssdp:discover"\r\n'
                      f'MX: 1\r\nST: {target}\r\n\r\n').encode(), ('239.255.255.250', 1900))
        try:
            while True:
                data, sender = s.recvfrom(4096)
                found = re.search(rb'(?im)^location:\s*(\S+)', data)
                if not found:
                    continue
                location = found.group(1).decode('ascii', 'replace')
                parsed = urllib.parse.urlparse(location)
                # A device describes itself: nothing on the network can send this to another host.
                if parsed.scheme == 'http' and parsed.hostname == sender[0]:
                    locations[location] = True
        except OSError:
            pass
    gateways = []
    for location in locations:
        try:
            with _opener.open(location, timeout=3) as reply:
                gateways += gateways_from_description(reply.read(262144), location)
        except (OSError, ET.ParseError, ValueError):
            continue
    return gateways


def port_is_free(port):
    """Whether this PC can listen on the port, TCP and UDP."""
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        with socket.socket(socket.AF_INET, kind) as s:
            try:
                s.bind(('', port))
            except OSError:
                return False
    return True


class HostSession:
    """One hosted game: opens the router's port, keeps it open, closes it when done."""

    def __init__(self, preferred_port=DEFAULT_PORT, discover=discover_gateways, stun=stun_public_ip,
                 lan_address=local_address):
        self.preferred_port = preferred_port if 0 < preferred_port < 65536 else DEFAULT_PORT
        self.discover, self.stun, self.lan_address = discover, stun, lan_address
        self.port = None
        self.status = LAN_ONLY
        self.lan_ip = self.public_ip = None
        self.code = None
        self.router_error = None
        self.gateway = None
        self.lease = LEASE_SECONDS
        self.stopping = threading.Event()
        self.closed = False
        self.lock = threading.Lock()

    def start(self):
        """Does the network work (seconds of it: not on the Tk thread) and fills in the fields."""
        self.lan_ip = self.lan_address()
        candidates = [p for p in range(self.preferred_port, min(self.preferred_port + PORT_TRIES, 65536))
                      if port_is_free(p)]
        if not candidates:
            raise OSError(f'ports {self.preferred_port} and up are in use on this PC')
        self.port = candidates[0]
        router_ip = None
        if self.lan_ip:
            router_ip = self.open_router_port(candidates)
        seen_ip = self.stun()
        best = seen_ip or router_ip
        self.public_ip = best if best and is_public(best) else None
        if self.gateway:
            # The router took the mapping; it only helps if the router is what the internet sees.
            behind = not (router_ip and is_public(router_ip)) or (seen_ip and seen_ip != router_ip)
            self.status = SHARED if behind else OPEN
        else:
            self.status = MANUAL if self.public_ip else LAN_ONLY
        address = self.public_ip or self.lan_ip or '127.0.0.1'
        self.code = encode_code(address, self.port, self.lan_ip)
        if self.gateway:
            atexit.register(self.close)
            threading.Thread(target=self.keep_open, name='UPnP lease', daemon=True).start()
        return self

    def open_router_port(self, ports):
        """Maps the first port the router takes; its external address, None when there is no way."""
        try:
            gateways = self.discover(self.lan_ip)
        except OSError as error:
            self.router_error = str(error)
            return None
        for gateway in gateways:
            client = self.client_address(gateway)
            try:
                external = gateway.external_ip()
                for port in ports:
                    try:
                        self.map_port(gateway, port, client)
                    except UpnpError as error:
                        if error.code in CONFLICT_ERRORS:
                            continue
                        raise
                    self.gateway, self.port = gateway, port
                    return external
                self.router_error = 'every port is taken on the router'
            except UpnpError as error:
                self.router_error = str(error)
        if not gateways and not self.router_error:
            self.router_error = 'no router answered'
        return None

    def client_address(self, gateway):
        return local_address(urllib.parse.urlparse(gateway.control_url).hostname) or self.lan_ip

    def map_port(self, gateway, port, client):
        """TCP and UDP together: the matching and the relay need both."""
        try:
            gateway.add('TCP', port, client, self.lease)
        except UpnpError as error:
            if error.code != ONLY_PERMANENT:
                raise
            self.lease = 0
            gateway.add('TCP', port, client, 0)
        try:
            gateway.add('UDP', port, client, self.lease)
        except UpnpError:
            try:
                gateway.delete('TCP', port)
            except UpnpError:
                pass
            raise

    def keep_open(self):
        if not self.lease:
            return
        while not self.stopping.wait(self.lease / 2):
            try:
                self.map_port(self.gateway, self.port, self.client_address(self.gateway))
            except UpnpError as error:
                self.router_error = str(error)

    def close(self):
        """Closes the port again. Safe to call twice, and at exit."""
        with self.lock:
            if self.closed:
                return
            self.closed = True
        self.stopping.set()
        if self.gateway:
            for protocol in ('TCP', 'UDP'):
                try:
                    self.gateway.delete(protocol, self.port)
                except UpnpError:
                    pass
        try:
            atexit.unregister(self.close)
        except Exception:  # noqa: BLE001 (interpreter shutting down)
            pass
