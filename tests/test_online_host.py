"""Join codes, STUN parsing and the UPnP conversation, against a fake router on loopback."""
import http.server
import re
import socket
import struct
import sys
import threading
import unittest

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
import online_host as oh  # noqa: E402

DESCRIPTION = '''<?xml version="1.0"?><root xmlns="urn:schemas-upnp-org:device-1-0"><device><serviceList>
<service><serviceType>urn:schemas-upnp-org:service:Layer3Forwarding:1</serviceType><controlURL>/l3</controlURL></service>
<service><serviceType>urn:schemas-upnp-org:service:WANPPPConnection:1</serviceType><controlURL>/ppp</controlURL></service>
<service><serviceType>urn:schemas-upnp-org:service:WANIPConnection:1</serviceType><controlURL>/ctl/ip</controlURL></service>
</serviceList></device></root>'''


class FakeRouter:
    """Just enough IGD: mappings by (protocol, port), with the refusals real routers make."""

    def __init__(self, external='93.184.216.9'):
        self.external = external
        self.mappings = {}
        self.taken = set()         # ports held by another PC: error 718
        self.permanent_only = False
        self.refuse_all = None     # an error code for every AddPortMapping
        self.refuse_udp = None
        self.calls = []
        router = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_POST(self):
                body = self.rfile.read(int(self.headers['Content-Length'])).decode()
                action = re.search(r'#(\w+)"', self.headers['SOAPAction']).group(1)
                fields = dict(re.findall(r'<(New\w+)>([^<]*)<', body))
                router.calls.append((action, fields))
                error, text = router.handle(action, fields)
                data = (f'<errorCode>{error}</errorCode><errorDescription>no</errorDescription>' if error else text).encode()
                self.send_response(500 if error else 200)
                self.send_header('Content-Length', str(len(data)))
                self.end_headers()
                self.wfile.write(data)

        self.server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.url = f'http://127.0.0.1:{self.server.server_address[1]}/ctl/ip'

    def handle(self, action, f):
        if action == 'GetExternalIPAddress':
            return 0, f'<NewExternalIPAddress>{self.external}</NewExternalIPAddress>'
        key = (f.get('NewProtocol'), int(f.get('NewExternalPort', 0)))
        if action == 'AddPortMapping':
            if self.refuse_all:
                return self.refuse_all, ''
            if key[0] == 'UDP' and self.refuse_udp:
                return self.refuse_udp, ''
            if key[1] in self.taken:
                return 718, ''
            if self.permanent_only and f['NewLeaseDuration'] != '0':
                return 725, ''
            self.mappings[key] = f
        elif action == 'DeletePortMapping':
            self.mappings.pop(key, None)
        return 0, ''

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def free_port():
    with socket.socket() as s:
        s.bind(('', 0))
        return s.getsockname()[1]


class Codes(unittest.TestCase):
    def test_round_trips(self):
        for address in [('93.184.216.9', 7788, '192.168.1.149'), ('93.184.216.9', 7788, None),
                        ('93.184.216.9', 7801, '192.168.0.2'), ('8.8.4.4', 7788, '10.1.2.3'),
                        ('1.0.0.1', 65535, '172.16.0.1'), ('0.0.0.1', 7788, None)]:
            self.assertEqual(oh.decode_code(oh.encode_code(*address)), address)

    def test_usual_code_is_short(self):
        code = oh.encode_code('93.184.216.9', 7788, '192.168.1.149')
        self.assertEqual(len(code.replace('-', '')), 12, code)
        self.assertRegex(code, r'^\w{4}-\w{4}-\w{4}$')

    def test_forgiving_typing(self):
        code = oh.encode_code('93.184.216.9', 7788, '192.168.1.149')
        wanted = oh.decode_code(code)
        self.assertEqual(oh.decode_code(code.lower().replace('-', ' ')), wanted)
        self.assertEqual(oh.decode_code(code.replace('0', 'O').replace('1', 'I')), wanted)

    def test_most_typos_are_caught(self):
        code = oh.encode_code('93.184.216.9', 7788, '192.168.1.149').replace('-', '')
        wrong = accepted = 0
        for i, c in enumerate(code):
            for other in oh.ALPHABET:
                if other == c:
                    continue
                wrong += 1
                try:
                    oh.decode_code(code[:i] + other + code[i + 1:])
                    accepted += 1
                except ValueError:
                    pass
        self.assertLess(accepted / wrong, 0.1)

    def test_rubbish_is_refused(self):
        for text in ('', 'hello', 'UUUU-UUUU', '----', '0000', 'ZZZZ-ZZZZ-ZZZZ'):
            with self.assertRaises(ValueError, msg=text):
                oh.decode_code(text)

    def test_candidates(self):
        code = oh.encode_code('93.184.216.9', 7801, '192.168.1.149')
        self.assertEqual(oh.join_candidates(code), ['192.168.1.149:7801', '93.184.216.9:7801'])
        self.assertEqual(oh.join_candidates(oh.encode_code('93.184.216.9')), ['93.184.216.9:7788'])
        self.assertEqual(oh.join_candidates(' wipeout.example.org '), ['wipeout.example.org:7788'])
        self.assertEqual(oh.join_candidates('93.184.216.5:9000'), ['93.184.216.5:9000'])
        for text in ('93.184.216.5:port', ':7788', 'nonsense'):
            with self.assertRaises(ValueError, msg=text):
                oh.join_candidates(text)


class Names(unittest.TestCase):
    def test_as_the_runtime_sanitizes(self):
        for typed, wanted in (('Alice', 'Alice'), ('Bob Smith', 'Bob_Smith'), ('  Al  ', 'Al1'[:2] + '2'),
                              ('a', 'a12'), ('x!y#z', 'xyz'), ('Ünï', 'nP2'[:1] + '12'),
                              ('A' * 20, 'A' * 16), ('a-b_c', 'a-b_c')):
            self.assertEqual(oh.online_name(typed), wanted, typed)

    def test_empty_differs_per_machine(self):
        self.assertRegex(oh.online_name('', 'PC-A'), r'^Player-[0-9a-f]{4}$')
        self.assertNotEqual(oh.online_name('', 'PC-A'), oh.online_name(' ', 'PC-B'))
        self.assertEqual(oh.online_name('', 'PC-A'), oh.online_name('!!!', 'PC-A'))


class Addresses(unittest.TestCase):
    def test_public(self):
        self.assertTrue(oh.is_public('8.8.8.8'))
        for ip in ('192.168.1.1', '10.0.0.1', '172.16.3.1', '100.64.0.1', '127.0.0.1', 'x'):
            self.assertFalse(oh.is_public(ip), ip)

    def test_stun_reply(self):
        tid = bytes(range(12))
        xored = bytes(a ^ b for a, b in zip(socket.inet_aton('93.184.216.9'), b'\x21\x12\xa4\x42'))
        other = struct.pack('>HH', 0x8022, 5) + b'hello\0\0\0'
        attr = struct.pack('>HHBBH', 0x0020, 8, 0, 1, 1234) + xored
        reply = struct.pack('>HHI', 0x0101, len(other) + len(attr), 0x2112A442) + tid + other + attr
        self.assertEqual(oh.parse_stun_response(reply, tid), '93.184.216.9')
        self.assertIsNone(oh.parse_stun_response(reply, bytes(12)))
        self.assertIsNone(oh.parse_stun_response(b'short', tid))

    def test_reachable_prefers_the_first(self):
        with socket.socket() as a, socket.socket() as b:
            for s in (a, b):
                s.bind(('127.0.0.1', 0))
                s.listen()
            first, second = (f'127.0.0.1:{s.getsockname()[1]}' for s in (a, b))
            dead = f'127.0.0.1:{free_port()}'
            self.assertEqual(oh.reachable_address([first, second], 1), first)
            self.assertEqual(oh.reachable_address([dead, second], 1), second)
            self.assertIsNone(oh.reachable_address([dead], 1))

    def test_description(self):
        found = oh.gateways_from_description(DESCRIPTION, 'http://192.168.1.1:5000/root.xml')
        self.assertEqual([g.control_url for g in found],
                         ['http://192.168.1.1:5000/ctl/ip', 'http://192.168.1.1:5000/ppp'])


class Hosting(unittest.TestCase):
    def setUp(self):
        self.router = FakeRouter()
        self.addCleanup(self.router.close)
        self.port = free_port()

    def session(self, stun='93.184.216.9', gateways=None):
        gateway = oh.Gateway(self.router.url, 'urn:schemas-upnp-org:service:WANIPConnection:1')
        session = oh.HostSession(self.port, discover=lambda lan: [gateway] if gateways is None else gateways,
                                 stun=lambda: stun, lan_address=lambda: '192.168.1.149')
        self.addCleanup(session.close)
        return session.start()

    def test_opens_both_protocols_and_closes_them(self):
        s = self.session()
        self.assertEqual(s.status, oh.OPEN)
        self.assertEqual(set(self.router.mappings), {('TCP', self.port), ('UDP', self.port)})
        f = self.router.mappings['UDP', self.port]
        self.assertEqual(f['NewInternalPort'], str(self.port))
        self.assertEqual(f['NewLeaseDuration'], str(oh.LEASE_SECONDS))
        self.assertEqual(oh.decode_code(s.code), ('93.184.216.9', self.port, '192.168.1.149'))
        s.close()
        s.close()
        self.assertEqual(self.router.mappings, {})

    def test_taken_port_moves_on(self):
        self.router.taken.add(self.port)
        s = self.session()
        self.assertEqual(s.status, oh.OPEN)
        self.assertGreater(s.port, self.port)
        self.assertEqual(oh.decode_code(s.code)[1], s.port)
        self.assertNotIn(('TCP', self.port), self.router.mappings)

    def test_half_a_mapping_is_not_left_behind(self):
        self.router.refuse_udp = 718
        s = self.session()
        self.assertEqual(s.status, oh.MANUAL)
        self.assertEqual(self.router.mappings, {})

    def test_permanent_only_router(self):
        self.router.permanent_only = True
        s = self.session()
        self.assertEqual((s.status, s.lease), (oh.OPEN, 0))
        self.assertEqual(self.router.mappings['TCP', s.port]['NewLeaseDuration'], '0')
        s.close()
        self.assertEqual(self.router.mappings, {})

    def test_router_refuses(self):
        self.router.refuse_all = 606
        s = self.session()
        self.assertEqual(s.status, oh.MANUAL)
        self.assertIn('606', s.router_error)
        self.assertEqual(oh.decode_code(s.code)[0], '93.184.216.9')

    def test_no_router(self):
        s = self.session(gateways=[])
        self.assertEqual(s.status, oh.MANUAL)
        self.assertEqual(s.port, self.port)

    def test_carrier_grade_nat(self):
        self.router.external = '100.64.3.4'
        self.assertEqual(self.session().status, oh.SHARED)

    def test_second_router_upstream(self):
        self.router.external = '192.168.0.20'
        self.assertEqual(self.session(stun='1.1.1.1').status, oh.SHARED)

    def test_cgnat_without_stun(self):
        self.router.external = '100.64.3.4'
        self.assertEqual(self.session(stun=None).status, oh.SHARED)

    def test_offline_gives_a_lan_code(self):
        s = self.session(stun=None, gateways=[])
        self.assertEqual(s.status, oh.LAN_ONLY)
        self.assertEqual(oh.join_candidates(s.code), [f'192.168.1.149:{self.port}'])

    def test_lease_is_renewed(self):
        s = self.session()
        s.close()
        s.closed = False
        s.stopping.clear()
        s.lease = 0.1
        self.router.calls.clear()
        thread = threading.Thread(target=s.keep_open, daemon=True)
        thread.start()
        thread.join(0.35)
        s.close()
        added = [c for c in self.router.calls if c[0] == 'AddPortMapping']
        self.assertGreaterEqual(len(added), 2)


if __name__ == '__main__':
    unittest.main()
