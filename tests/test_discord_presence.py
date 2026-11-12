import json
import os
import socket
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock
from pathlib import Path

from paths import ROOT
sys.path.insert(0, str(ROOT / 'scripts'))
import discord_presence as dp  # noqa: E402


class FakeTransport:
    def __init__(self, replies):
        self.incoming = b''.join(dp.pack(dp.FRAME, reply) for reply in replies)
        self.written = []
        self.closed = False

    def write(self, data):
        self.written.append(data)

    def read(self, size):
        chunk, self.incoming = self.incoming[:size], self.incoming[size:]
        return chunk

    def close(self):
        self.closed = True


class LiveTransport(FakeTransport):
    def __init__(self, replies):
        super().__init__(replies)
        self.published = threading.Event()
        self.pong = threading.Event()
        self.reading = threading.Event()

    def read(self, size):
        self.reading.set()
        if self.incoming:
            chunk = super().read(size)
            if not self.incoming:
                self.published.set()
            return chunk
        if self.closed:
            return b''
        time.sleep(0.01)
        raise TimeoutError

    def write(self, data):
        super().write(data)
        if dp.HEADER.unpack(data[:8])[0] == 4:
            self.pong.set()


def decode(data):
    opcode, length = dp.HEADER.unpack(data[:dp.HEADER.size])
    return opcode, json.loads(data[dp.HEADER.size:dp.HEADER.size + length])


class Framing(unittest.TestCase):
    def test_pack_has_opcode_length_and_utf8_body(self):
        data = dp.pack(dp.FRAME, {'a': 'ñ'})
        opcode, length = dp.HEADER.unpack(data[:8])
        self.assertEqual(opcode, dp.FRAME)
        self.assertEqual(length, len(data) - 8)
        self.assertEqual(json.loads(data[8:]), {'a': 'ñ'})

    def test_read_frame_reassembles_split_reads(self):
        class Slow(FakeTransport):
            def read(self, size):
                return super().read(min(size, 3))
        transport = Slow([{'evt': 'READY'}])
        self.assertEqual(dp.read_frame(transport), (dp.FRAME, {'evt': 'READY'}))

    def test_ping_echoes_raw_bytes_before_the_next_frame(self):
        transport = FakeTransport([{'evt': 'READY'}])
        payload = b'\xff\x00ping'
        transport.incoming = dp.HEADER.pack(3, len(payload)) + payload + transport.incoming
        self.assertEqual(dp.read_frame(transport), (dp.FRAME, {'evt': 'READY'}))
        self.assertEqual(transport.written, [dp.HEADER.pack(4, len(payload)) + payload])

    def test_partial_frame_survives_a_read_timeout(self):
        class Interrupted(FakeTransport):
            calls = 0

            def read(self, size):
                self.calls += 1
                if self.calls == 2:
                    raise TimeoutError
                return super().read(min(size, 3))

        self.assertEqual(dp.read_frame(Interrupted([{'evt': 'READY'}])),
                         (dp.FRAME, {'evt': 'READY'}))

    def test_read_frame_raises_when_connection_ends(self):
        with self.assertRaises(ConnectionError):
            dp.read_frame(FakeTransport([]))


class Activity(unittest.TestCase):
    def test_discords_own_entry_for_the_game_is_used(self):
        self.assertEqual(dp.CLIENT_ID, '1439067084456726588')

    def test_without_a_state_the_start_time_cover_and_port_show(self):
        value = dp.activity(100.9)
        self.assertEqual(value['timestamps'], {'start': 100})
        self.assertEqual(value['details'], 'Starting the game')
        self.assertNotIn('state', value)
        self.assertNotIn('party', value)
        self.assertEqual(value['assets'], {'large_image': dp.COVER_IMAGE, 'large_text': dp.GAME_NAME,
                                           'small_image': dp.PORT_IMAGE, 'small_text': dp.PORT_NAME})
        self.assertEqual(value['buttons'], [{'label': 'Get the PC port', 'url': dp.PORT_URL}])
        self.assertEqual(dp.activity(5, 'not a state')['details'], 'Starting the game')

    def test_menus_and_races_name_the_scene_track_mode_and_game(self):
        self.assertEqual(dp.describe({'scene': 'menu'}), ('In the menus', ''))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'vinetak', 'mode': ''}), ('Racing on Vineta K', ''))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'sebencoclimb', 'mode': 'zone'}),
                         ('Zone on Sebenco Climb', 'WipEout HD'))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'talonsjunction', 'mode': 'elim'}),
                         ("Eliminator on Talon's Junction", 'WipEout HD Fury'))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'altima', 'mode': 'zbattle'}),
                         ('Zone Battle on Altima', 'WipEout 2048'))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'zone', 'mode': ''}), ('Zone', 'WipEout HD Fury'))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'det', 'mode': 'det'}), ('Detonator', 'WipEout HD Fury'))
        self.assertEqual(dp.describe({'scene': 'race', 'track': 'new_track', 'mode': 'odd'}), ('Racing on New Track', ''))
        self.assertEqual(dp.describe({'scene': 'race'}), ('Racing', ''))

    def test_every_track_bank_has_a_name_and_a_game(self):
        for key, (name, game) in dp.TRACKS.items():
            self.assertRegex(key, '^[a-z0-9]+$')
            self.assertTrue(name or key in ('zone', 'det'), key)
            self.assertIn(game, ('', dp.HD, dp.FURY, dp.G2048))

    def test_online_room_is_the_party(self):
        value = dp.activity(1, {'scene': 'menu', 'room': [2, 8, '1000']})
        self.assertEqual(value['details'], 'In an online lobby')
        self.assertEqual(value['party'], {'id': 'room-1000', 'size': [2, 8]})
        value = dp.activity(1, {'scene': 'race', 'track': 'metropia', 'mode': 'zone', 'room': [3, 2, '7']})
        self.assertEqual(value['details'], 'Zone online on Metropia')
        self.assertEqual(value['party'], {'id': 'room-7', 'size': [3, 3]})  # never more members than slots
        for empty in ([0, 0, '0'], [1, 8], 'x', ['1', '8', '1']):
            self.assertNotIn('party', dp.activity(1, {'scene': 'menu', 'room': empty}))
            self.assertEqual(dp.activity(1, {'scene': 'menu', 'room': empty})['details'], 'In the menus')

    def test_the_song_replaces_the_port_badge_text(self):
        value = dp.activity(1, {'scene': 'race', 'track': 'sol', 'song': ' Artist - Title '})
        self.assertEqual(value['assets']['small_text'], '\u266a Artist - Title')
        self.assertEqual(dp.activity(1, {'song': ''})['assets']['small_text'], dp.PORT_NAME)
        self.assertEqual(dp.activity(1, {'song': 3})['assets']['small_text'], dp.PORT_NAME)
        self.assertEqual(len(dp.activity(1, {'song': 'x' * 300})['assets']['small_text']), dp.TEXT_LIMIT)
        self.assertEqual(len(dp.describe({'scene': 'race', 'track': 'y' * 300})[0]), dp.TEXT_LIMIT)

    def test_clearing_omits_the_activity(self):
        _, body = decode(dp.set_activity(7))
        self.assertEqual(body['args'], {'pid': 7})
        self.assertEqual(body['cmd'], 'SET_ACTIVITY')


class PresenceLifecycle(unittest.TestCase):
    def setUp(self):
        self.env = mock.patch.dict(os.environ, {}, clear=False)
        self.env.start()
        os.environ.pop('GOW3_DISCORD', None)
        os.environ.pop('GOW3_DISCORD_ID', None)

    def tearDown(self):
        self.env.stop()

    def test_disabled_without_client_id(self):
        with mock.patch.object(dp, 'CLIENT_ID', ''), mock.patch.object(dp, 'open_transport') as opened:
            presence = dp.Presence()
            presence.close()
        opened.assert_not_called()
        self.assertIsNone(presence.thread)

    def test_disabled_by_switch(self):
        os.environ['GOW3_DISCORD'] = '0'
        with mock.patch.object(dp, 'open_transport') as opened:
            presence = dp.Presence(client_id='123')
            presence.close()
        opened.assert_not_called()

    def test_handshake_activity_and_clean_close(self):
        transport = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', return_value=transport):
            presence = dp.Presence(client_id='123')
            self.addCleanup(presence.close)
            self.assertTrue(transport.published.wait(1))
            presence.close()
        frames = [decode(data) for data in transport.written]
        self.assertEqual(frames[0], (dp.HANDSHAKE, {'v': 1, 'client_id': '123'}))
        opcode, body = frames[1]
        self.assertEqual(opcode, dp.FRAME)
        self.assertEqual(body['args']['pid'], os.getpid())
        self.assertEqual(body['args']['activity'], dp.activity(presence.started))
        self.assertNotIn('activity', frames[2][1]['args'])
        self.assertEqual(frames[3][0], dp.CLOSE)
        self.assertTrue(transport.closed)

    def test_reconnect_republishes_original_start_time(self):
        first = FakeTransport([{'evt': 'READY'}, {'evt': None}])
        second = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', side_effect=[first, second]) as opened, \
                mock.patch.object(dp, 'RETRY_SECONDS', 0.01):
            presence = dp.Presence(client_id='123')
            try:
                self.assertTrue(second.published.wait(1), 'presence was not republished')
                self.assertEqual(opened.call_count, 2)
                self.assertTrue(first.closed)
                self.assertEqual(decode(first.written[1])[1]['args']['activity'],
                                 decode(second.written[1])[1]['args']['activity'])
            finally:
                presence.close()

    def test_connected_session_answers_ping_and_keeps_listening(self):
        transport = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', return_value=transport):
            presence = dp.Presence(client_id='123')
            try:
                self.assertTrue(transport.published.wait(1))
                transport.incoming += dp.pack(3, {'probe': 'alive'})
                self.assertTrue(transport.pong.wait(1), 'PING was not answered')
                self.assertTrue(presence.thread.is_alive())
                self.assertIn((4, {'probe': 'alive'}), [decode(x) for x in transport.written])
            finally:
                presence.close()

    def test_close_interrupts_a_silent_handshake_without_reconnecting(self):
        transport = LiveTransport([])
        with mock.patch.object(dp, 'open_transport', return_value=transport) as opened:
            presence = dp.Presence(client_id='123')
            self.assertTrue(transport.reading.wait(1))
            started = time.monotonic()
            presence.close()
            self.assertLess(time.monotonic() - started, 3)
            self.assertFalse(presence.thread.is_alive())
            self.assertTrue(transport.closed)
            self.assertEqual(opened.call_count, 1)

    def test_native_pid_is_used_for_publish_and_clear(self):
        transport = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', return_value=transport):
            presence = dp.Presence(client_id='123', pid=4321)
            try:
                self.assertTrue(transport.published.wait(1))
            finally:
                presence.close()
        self.assertEqual(decode(transport.written[1])[1]['args']['pid'], 4321)
        self.assertEqual(decode(transport.written[2])[1]['args'], {'pid': 4321})

    def test_rejected_handshake_closes_and_retries_later(self):
        transport = FakeTransport([{'evt': 'ERROR'}])
        with mock.patch.object(dp, 'open_transport', return_value=transport), \
                mock.patch.object(dp, 'RETRY_SECONDS', 60):
            presence = dp.Presence(client_id='123')
            for _ in range(100):
                if transport.closed:
                    break
                presence.thread.join(0.05)
            presence.close()
        self.assertTrue(transport.closed)
        self.assertIsNone(presence.transport)

    def test_discord_not_running_does_not_raise(self):
        with mock.patch.object(dp, 'open_transport', return_value=None), \
                mock.patch.object(dp, 'RETRY_SECONDS', 60):
            presence = dp.Presence(client_id='123')
            presence.close()
        self.assertFalse(presence.thread.is_alive())

    def test_close_without_connection_is_harmless(self):
        presence = dp.Presence(client_id='')
        presence.close()
        presence.close()


class GameProcessPresence(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(ROOT))
        import run
        import patches

        self.run = run
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        folder = Path(self.temp.name)
        game = folder / 'game'
        game.mkdir()
        (game / 'eboot.bin').write_bytes(b'fixture')
        probe = folder / 'probe.exe'
        probe.write_bytes(b'fixture')
        self.addCleanup(os.chdir, os.getcwd())
        self.process = mock.Mock(pid=4321)
        self.process.wait.return_value = 17
        self.presence = mock.Mock()
        self.factory = self.enterContext(mock.patch.object(dp, 'Presence', return_value=self.presence))
        self.enterContext(mock.patch.dict(os.environ, {
            'GOW3_GAME_DIR': str(game), 'GOW3_DATA_DIR': str(folder),
            'GOW3_CONFIG': str(folder / 'gow3.ini'), 'GOW3_DISCORD': '1',
        }, clear=True))
        self.enterContext(mock.patch.object(run, 'PORT', folder))
        self.enterContext(mock.patch.object(run, 'find_executable', return_value=probe))
        self.enterContext(mock.patch.object(run, 'run_script', return_value=str(game)))
        self.enterContext(mock.patch.object(patches, 'game_profile_for', return_value=None))
        self.enterContext(mock.patch.object(run.subprocess, 'run', return_value=mock.Mock(
            returncode=0, stdout='CPU: complete\n')))
        self.opened = self.enterContext(mock.patch.object(run.subprocess, 'Popen', return_value=self.process))
        self.enterContext(mock.patch.object(sys, 'argv', ['run.py']))

    def test_game_pid_is_used_and_presence_closes_after_wait(self):
        self.assertEqual(self.run.main(), 17)
        self.opened.assert_called_once()
        self.factory.assert_called_once_with(pid=4321, state_file=Path(self.temp.name) / 'out' / 'presence.json')
        self.assertEqual(os.environ['GOW3_PRESENCE_FILE'], str(Path(self.temp.name) / 'out' / 'presence.json'))

    def test_switched_off_the_runtime_gets_no_state_file(self):
        os.environ['GOW3_DISCORD'] = '0'
        self.assertEqual(self.run.main(), 17)
        self.factory.assert_called_once_with(pid=4321, state_file=None)
        self.assertNotIn('GOW3_PRESENCE_FILE', os.environ)
        self.process.wait.assert_called_once()
        self.presence.close.assert_called_once()

    def test_presence_failure_does_not_interrupt_the_game(self):
        self.factory.side_effect = RuntimeError('thread unavailable')
        self.assertEqual(self.run.main(), 17)
        self.process.wait.assert_called_once()

    def test_wait_exception_still_cleans_up_presence(self):
        self.process.wait.side_effect = KeyboardInterrupt
        with self.assertRaises(KeyboardInterrupt):
            self.run.main()
        self.presence.close.assert_called_once()


class StateFile(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name) / 'presence.json'
        self.enterContext(mock.patch.dict(os.environ, {'GOW3_DISCORD': '1'}))
        self.enterContext(mock.patch.object(dp, 'STATE_SECONDS', 0.01))

    def activities(self, transport):
        return [decode(data)[1]['args'].get('activity') for data in transport.written
                if dp.HEADER.unpack(data[:8])[0] == dp.FRAME and decode(data)[1].get('cmd') == 'SET_ACTIVITY']

    def wait_for(self, transport, count):
        deadline = time.monotonic() + 2
        while time.monotonic() < deadline:
            if len(self.activities(transport)) >= count:
                return self.activities(transport)
            time.sleep(0.01)
        self.fail(f'{count} activities were not published: {self.activities(transport)}')

    def write(self, text):
        temp = self.path.with_suffix('.tmp')
        temp.write_text(text, encoding='utf-8')
        os.replace(temp, self.path)

    def test_changes_are_published_and_a_half_written_file_waits(self):
        transport = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', return_value=transport):
            presence = dp.Presence(client_id='123', state_file=self.path)
            try:
                first = self.wait_for(transport, 1)[0]
                self.assertEqual(first['details'], 'Starting the game')
                self.write('{"scene":"menu","track":"","mode":"","room":[0,0,"0"],"song":""}\n')
                self.assertEqual(self.wait_for(transport, 2)[1]['details'], 'In the menus')
                self.write('{"scene":"race","track":"vin')  # cut short: ignored until it is whole
                time.sleep(0.1)
                self.assertEqual(len(self.activities(transport)), 2)
                self.write('{"scene":"race","track":"vinetak","mode":"zone","room":[2,8,"1000"],"song":"A - B"}\n')
                third = self.wait_for(transport, 3)[2]
                self.assertEqual(third['details'], 'Zone online on Vineta K')
                self.assertEqual(third['party'], {'id': 'room-1000', 'size': [2, 8]})
                self.assertEqual(third['assets']['small_text'], '\u266a A - B')
                self.write('{"scene":"race","track":"vinetak","mode":"zone","room":[2,8,"1000"],"song":"A - B"}\n')
                time.sleep(0.1)
                self.assertEqual(len(self.activities(transport)), 3)  # rewritten unchanged: nothing to publish
                self.assertTrue(presence.thread.is_alive())
            finally:
                presence.close()

    def test_a_reconnection_publishes_the_current_state(self):
        self.write('{"scene":"race","track":"altima","mode":"","room":[0,0,"0"],"song":""}\n')
        first = FakeTransport([{'evt': 'READY'}, {'evt': None}])
        second = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', side_effect=[first, second]), \
                mock.patch.object(dp, 'RETRY_SECONDS', 0.01):
            presence = dp.Presence(client_id='123', state_file=self.path)
            try:
                self.assertEqual(self.wait_for(second, 1)[0]['details'], 'Racing on Altima')
                self.assertEqual(self.activities(first)[0]['details'], 'Racing on Altima')
            finally:
                presence.close()

    def test_without_a_state_file_nothing_is_polled(self):
        transport = LiveTransport([{'evt': 'READY'}, {'evt': None}])
        with mock.patch.object(dp, 'open_transport', return_value=transport), \
                mock.patch.object(os, 'stat', wraps=os.stat) as stat:
            presence = dp.Presence(client_id='123')
            try:
                self.wait_for(transport, 1)
                time.sleep(0.05)
                self.assertFalse(presence.load_state())
                self.assertFalse(any(str(self.path) in str(call) for call in stat.call_args_list))
            finally:
                presence.close()


class SocketIntegration(unittest.TestCase):
    def test_real_socket_reconnect_ping_and_shutdown(self):
        from concurrent.futures import ThreadPoolExecutor

        pong = threading.Event()
        starts = []
        with socket.socket() as listener:
            listener.bind(('127.0.0.1', 0))
            listener.listen()
            listener.settimeout(2)

            def serve():
                for index in range(2):
                    conn, _ = listener.accept()
                    with conn:
                        conn.settimeout(2)
                        transport = dp.SocketTransport.__new__(dp.SocketTransport)
                        transport.sock = conn
                        hello = dp.pack(dp.HANDSHAKE, {'v': 1, 'client_id': '123'})
                        self.assertEqual(dp.read_exact(transport, len(hello)), hello)
                        transport.write(dp.pack(dp.FRAME, {'evt': 'READY'}))
                        starts.append(dp.read_frame(transport)[1]['args']['activity']['timestamps']['start'])
                        transport.write(dp.pack(dp.FRAME, {'evt': None}))
                        if index == 0:
                            continue
                        transport.write(dp.HEADER.pack(3, 2) + b'\xff\x00')
                        self.assertEqual(dp.read_exact(transport, 10), dp.HEADER.pack(4, 2) + b'\xff\x00')
                        pong.set()
                        self.assertNotIn('activity', dp.read_frame(transport)[1]['args'])
                        self.assertEqual(dp.read_exact(transport, 10), dp.pack(dp.CLOSE, {}))

            def connect(stop):
                transport = dp.SocketTransport.__new__(dp.SocketTransport)
                transport.sock = socket.create_connection(listener.getsockname(), timeout=dp.POLL_SECONDS)
                return transport

            with ThreadPoolExecutor(max_workers=1) as executor, \
                    mock.patch.dict(os.environ, {'GOW3_DISCORD': '1'}), \
                    mock.patch.object(dp, 'open_transport', side_effect=connect), \
                    mock.patch.object(dp, 'RETRY_SECONDS', 0.01):
                server = executor.submit(serve)
                presence = dp.Presence(client_id='123')
                try:
                    self.assertTrue(pong.wait(2))
                finally:
                    presence.close()
                server.result(timeout=3)
                self.assertFalse(presence.thread.is_alive())
                self.assertEqual(starts, [int(presence.started)] * 2)


@unittest.skipUnless(sys.platform == 'win32', 'Windows named pipes')
class WindowsPipeTransport(unittest.TestCase):
    def setUp(self):
        import ctypes
        import uuid

        self.ctypes = ctypes
        self.kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.kernel32.CreateNamedPipeW.restype = ctypes.c_void_p
        path = rf'\\.\pipe\gow3-rpc-test-{uuid.uuid4()}'
        handle = self.kernel32.CreateNamedPipeW(ctypes.c_wchar_p(path), 3, 0, 1, 1024, 1024, 0, None)
        self.assertNotEqual(handle, ctypes.c_void_p(-1).value)
        self.pipe = ctypes.c_void_p(handle)
        self.addCleanup(self.kernel32.CloseHandle, self.pipe)
        self.transport = dp.PipeTransport(path)
        self.addCleanup(self.transport.close)
        connected = self.kernel32.ConnectNamedPipe(self.pipe, None)
        self.assertTrue(connected or ctypes.get_last_error() == 535)

    def test_native_pipe_reads_and_writes_without_blocking(self):
        c = self.ctypes
        count = c.c_uint32()
        self.transport.write(b'client')
        buf = c.create_string_buffer(6)
        self.assertTrue(self.kernel32.ReadFile(self.pipe, buf, 6, c.byref(count), None))
        self.assertEqual(buf.raw, b'client')
        self.assertTrue(self.kernel32.WriteFile(self.pipe, b'server', 6, c.byref(count), None))
        self.assertEqual(self.transport.read(6), b'server')
        started = time.monotonic()
        with self.assertRaises(TimeoutError):
            self.transport.read(1)
        self.assertLess(time.monotonic() - started, 1)
        started = time.monotonic()
        with self.assertRaises(OSError):
            self.transport.write(b'x' * (1024 * 1024))
        self.assertLess(time.monotonic() - started, 1)

    def test_close_stops_worker_waiting_for_a_native_pipe_handshake(self):
        with mock.patch.dict(os.environ, {'GOW3_DISCORD': '1'}), \
                mock.patch.object(dp, 'open_transport', return_value=self.transport) as opened:
            presence = dp.Presence(client_id='123')
            try:
                available = self.ctypes.c_uint32()
                deadline = time.monotonic() + 1
                while time.monotonic() < deadline:
                    self.assertTrue(self.kernel32.PeekNamedPipe(
                        self.pipe, None, 0, None, self.ctypes.byref(available), None))
                    if available.value:
                        break
                    time.sleep(0.01)
                self.assertGreater(available.value, 0)
                started = time.monotonic()
                presence.close()
                self.assertLess(time.monotonic() - started, 3)
                self.assertFalse(presence.thread.is_alive())
                self.assertEqual(opened.call_count, 1)
            finally:
                presence.close()


class LauncherSwitch(unittest.TestCase):
    def setUp(self):
        sys.path.insert(0, str(ROOT / 'launcher'))
        import gow3_launcher_win as launcher
        self.launcher = launcher

    def environment(self, **settings):
        values = {**self.launcher.APP_DEFAULTS, **settings}
        with mock.patch.object(self.launcher, 'game_profile_of', return_value=(None, None)):
            return self.launcher.game_environment(values)

    def test_off_by_default(self):
        self.assertFalse(self.launcher.APP_DEFAULTS['discord_rpc'])
        self.assertEqual(self.environment()['GOW3_DISCORD'], '0')

    def test_checked_enables_it(self):
        self.assertEqual(self.environment(discord_rpc=True)['GOW3_DISCORD'], '1')

    def test_label_is_translated(self):
        import gow3_lang
        self.assertEqual(gow3_lang.table('es')['Show Discord Rich Presence'], 'Mostrar Discord Rich Presence')
        self.assertIn('Discord', gow3_lang.table('pt')['Show Discord Rich Presence'])
        hint = ('Shows the game, the track and mode you play, your online lobby and the time played on your Discord '
                'profile, as "Playing WipEout: Omega Collection" with its game page.')
        self.assertTrue(gow3_lang.table('es')[hint].startswith('Muestra en tu perfil de Discord el juego, la pista'))
        self.assertTrue(gow3_lang.table('pt')[hint].startswith('Mostra no seu perfil do Discord o jogo, a pista'))
        self.assertIn("'discord_rpc', 'app'", (ROOT / 'launcher' / 'gow3_launcher_win.py').read_text(encoding='utf-8'))
        self.assertEqual(gow3_lang.table('es')['Game multisampling'], 'Multisampling del juego')


if __name__ == '__main__':
    unittest.main()
