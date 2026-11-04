"""Discord Rich Presence for the game: run.py starts it beside the game process and closes it when
the game ends. It talks to the Discord client on this PC over Discord's IPC socket (a named pipe on
Windows), as discord-rpc does, with the game's own entry in Discord's game database: the status
reads "Playing WipEout: Omega Collection" with Discord's icon for it and opens its game page.

What the status shows comes from the runtime (src/runtime_presence.c): the scene the game is in
(the menus, a race with its track and mode), the online room and the custom soundtrack's song,
written to a small JSON file (GOW3_PRESENCE_FILE) whenever one of them changes. This client polls
the file once a second. Nothing leaves the PC except what Discord itself shows to the player's
friends.

GOW3_DISCORD=0 turns the presence off (the launcher's switch); GOW3_DISCORD_ID names another
Discord application, whose name and icon then show instead."""
import json
import os
import socket
import struct
import sys
import threading
import time
import uuid

# Discord's own entry for WipEout: Omega Collection (the game page in Discord's library). Any
# program may publish a status with it; the status then carries that page's name and icon.
CLIENT_ID = '1439067084456726588'
GAME_NAME = 'WipEout Omega Collection'
# The game page's cover art, and the port's icon as the small badge: Discord fetches both itself.
COVER_IMAGE = 'https://cdn.discordapp.com/app-icons/1439067084456726588/02c79423763c1c8dc30e8a63858f3d15.png?size=512'
PORT_IMAGE = 'https://raw.githubusercontent.com/AbdelmoumenDeghbouche/wipeout_pc/main/launcher/gow3.png'
PORT_NAME = 'WipEout Omega Collection for PC (wipeout_pc)'
PORT_URL = 'https://github.com/AbdelmoumenDeghbouche/wipeout_pc'
TEXT_LIMIT = 128  # Discord's limit for the details, state and image texts
RETRY_SECONDS = 15
STATE_SECONDS = 1
JOIN_SECONDS = 3
POLL_SECONDS = 0.1
READ_SECONDS = 5
WINDOWS = sys.platform == 'win32'
HEADER = struct.Struct('<II')
HANDSHAKE, FRAME, CLOSE, PING, PONG = range(5)

# The environment sound bank the game loads for a race names its track (src/runtime_presence.c
# keeps the part after "env" and the number): the track's name and the game it belongs to.
HD, FURY, G2048 = 'WipEout HD', 'WipEout HD Fury', 'WipEout 2048'
TRACKS = {
    'vinetak': ('Vineta K', ''),  # HD and 2048 both have one, on the same bank
    'metropia': ('Metropia', HD), 'moatherma': ('Moa Therma', HD), 'chengou': ('Chenghou Project', HD),
    'ubermall': ('Ubermall', HD), 'sebencoclimb': ('Sebenco Climb', HD), 'sol2': ('Sol 2', HD),
    'anulphapass': ('Anulpha Pass', HD),
    'talonsjunction': ("Talon's Junction", FURY), 'amphesium': ('The Amphiseum', FURY),
    'modesto': ('Modesto Heights', FURY), 'techdera': ('Tech De Ra', FURY),
    'zone': ('', FURY), 'det': ('', FURY),  # one bank for the zone tracks, one for Detonator
    'altima': ('Altima', G2048), 'arena': ('Rockway Stadium', G2048), 'bridge': ('Capital Reach', G2048),
    'cathedral': ('Downtown', G2048), 'mall': ('Queens Mall', G2048), 'park': ('Metro Park', G2048),
    'sol': ('Sol', G2048), 'square': ('Unity Square', G2048), 'subway': ('Subway', G2048),
    'tower': ('Empire Climb', G2048),
}
MODES = {'zone': 'Zone', 'zbattle': 'Zone Battle', 'elim': 'Eliminator', 'det': 'Detonator'}


def pack(opcode, payload):
    body = json.dumps(payload, separators=(',', ':'), ensure_ascii=False).encode('utf-8')
    return HEADER.pack(opcode, len(body)) + body


def read_exact(transport, size, stop=None, timeout=READ_SECONDS, idle=None):
    """`idle` runs each time the transport has nothing, while no byte of this read has arrived."""
    deadline = time.monotonic() + timeout if timeout is not None else None
    data = bytearray()
    while len(data) < size:
        if stop is not None and stop.is_set():
            raise ConnectionAbortedError('Discord presence stopped')
        if deadline is not None and time.monotonic() >= deadline:
            raise TimeoutError('Discord response timed out')
        try:
            chunk = transport.read(size - len(data))
        except TimeoutError:
            if idle is not None and not data:
                idle()
            continue
        if not chunk:
            raise ConnectionError('Discord closed the connection')
        data.extend(chunk)
    return bytes(data)


def read_frame(transport, stop=None, timeout=READ_SECONDS, idle=None):
    while True:
        opcode, length = HEADER.unpack(read_exact(transport, HEADER.size, stop, timeout, idle))
        if length > 1024 * 1024:
            raise ValueError('Discord frame is too large')
        body = read_exact(transport, length, stop) if length else b''
        if opcode == PING:
            transport.write(HEADER.pack(PONG, length) + body)
            continue
        if opcode == PONG:
            continue
        if opcode == CLOSE:
            raise ConnectionError('Discord closed the connection')
        if opcode != FRAME:
            raise ValueError('Unexpected Discord opcode')
        data = json.loads(body) if body else {}
        if not isinstance(data, dict):
            raise ValueError('Discord frame must contain an object')
        return opcode, data


def room(state):
    """(members, slots, id) of the online room the game is in, or None."""
    value = state.get('room')
    if not isinstance(value, list) or len(value) != 3 or not all(isinstance(n, int) for n in value[:2]):
        return None
    members, slots, room_id = value
    if members < 1:
        return None
    return members, max(slots, members), str(room_id)


def describe(state):
    """The two lines of the status: what the game does, and which game of the collection."""
    scene = state.get('scene')
    online = room(state) is not None
    if scene == 'race':
        key = state.get('track') if isinstance(state.get('track'), str) else ''
        track, game = TRACKS.get(key, (key.replace('_', ' ').title(), ''))
        mode = MODES.get(state.get('mode'), '')
        if key == 'zone':
            mode = mode or 'Zone'
        verb = mode or 'Racing'
        if online:
            verb += ' online'
        details = f'{verb} on {track}' if track else verb
        return details[:TEXT_LIMIT], game[:TEXT_LIMIT]
    if scene == 'menu':
        return ('In an online lobby' if online else 'In the menus'), ''
    return 'Starting the game', ''


def activity(started, state=None):
    """The activity Discord shows: the time played since `started`, and the game's state when known."""
    state = state if isinstance(state, dict) else {}
    details, line = describe(state)
    value = {'details': details, 'timestamps': {'start': int(started)},
             'assets': {'large_image': COVER_IMAGE, 'large_text': GAME_NAME,
                        'small_image': PORT_IMAGE, 'small_text': PORT_NAME},
             'buttons': [{'label': 'Get the PC port', 'url': PORT_URL}]}
    if line:
        value['state'] = line
    party = room(state)
    if party:
        members, slots, room_id = party
        value['party'] = {'id': f'room-{room_id}', 'size': [members, slots]}
    song = state.get('song')
    if isinstance(song, str) and song.strip():
        value['assets']['small_text'] = f'\u266a {song.strip()}'[:TEXT_LIMIT]
    return value


def set_activity(pid, value=None):
    args = {'pid': pid}
    if value is not None:
        args['activity'] = value
    return pack(FRAME, {'cmd': 'SET_ACTIVITY', 'args': args, 'nonce': str(uuid.uuid4())})


class PipeTransport:
    def __init__(self, path):
        import ctypes
        import msvcrt

        self.handle = open(path, 'r+b', buffering=0)
        self.kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self.pipe = ctypes.c_void_p(msvcrt.get_osfhandle(self.handle.fileno()))
        # Nonblocking writes also prevent a stalled Discord client from holding up shutdown.
        mode = ctypes.c_uint32(1)  # PIPE_NOWAIT | PIPE_READMODE_BYTE
        if not self.kernel32.SetNamedPipeHandleState(self.pipe, ctypes.byref(mode), None, None):
            error = ctypes.WinError(ctypes.get_last_error())
            self.handle.close()
            raise error

    def write(self, data):
        if self.handle.write(data) != len(data):
            raise ConnectionError('Discord pipe could not accept the frame')

    def read(self, size):
        import ctypes

        deadline = time.monotonic() + POLL_SECONDS
        available = ctypes.c_uint32()
        while True:
            if not self.kernel32.PeekNamedPipe(self.pipe, None, 0, None, ctypes.byref(available), None):
                raise ctypes.WinError(ctypes.get_last_error())
            if available.value:
                return self.handle.read(min(size, available.value))
            if time.monotonic() >= deadline:
                raise TimeoutError
            time.sleep(0.01)

    def close(self):
        self.handle.close()


class SocketTransport:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.settimeout(POLL_SECONDS)
        try:
            self.sock.connect(path)
        except OSError:
            self.sock.close()
            raise

    def write(self, data):
        self.sock.sendall(data)

    def read(self, size):
        return self.sock.recv(size)

    def close(self):
        self.sock.close()


def candidates():
    if WINDOWS:
        return [rf'\\?\pipe\discord-ipc-{index}' for index in range(10)]
    bases = [os.environ.get(name) for name in ('XDG_RUNTIME_DIR', 'TMPDIR', 'TMP', 'TEMP')] + ['/tmp']
    paths = []
    for base in dict.fromkeys(base for base in bases if base):
        for sub in ('', 'app/com.discordapp.Discord', 'snap.discord'):
            for index in range(10):
                paths.append(os.path.join(base, sub, f'discord-ipc-{index}'))
    return paths


def open_transport(stop=None):
    kind = PipeTransport if WINDOWS else SocketTransport
    for path in candidates():
        if stop is not None and stop.is_set():
            break
        try:
            return kind(path)
        except OSError:
            continue
    return None


def handshake(transport, client_id, stop=None):
    transport.write(pack(HANDSHAKE, {'v': 1, 'client_id': str(client_id)}))
    opcode, data = read_frame(transport, stop)
    if opcode != FRAME or data.get('evt') != 'READY':
        raise ConnectionError('Discord rejected the handshake')


def send_activity(transport, value, pid, stop=None):
    transport.write(set_activity(pid, value))
    _, data = read_frame(transport, stop)
    if data.get('evt') == 'ERROR':
        raise ConnectionError('Discord rejected the activity')


class Presence:
    def __init__(self, client_id=None, pid=None, state_file=None):
        self.transport = None
        self.thread = None
        self.stop = threading.Event()
        self.started = time.time()
        self.pid = os.getpid() if pid is None else pid
        self.state_file = str(state_file) if state_file else None
        self.state = None  # the state file's last content
        self.stamp = None  # the file's (mtime, size) when it was read
        self.checked = 0.0
        self.client_id = client_id or os.environ.get('GOW3_DISCORD_ID') or CLIENT_ID
        if not self.client_id or os.environ.get('GOW3_DISCORD') == '0':
            return
        self.thread = threading.Thread(target=self.run, name='discord-presence', daemon=True)
        self.thread.start()

    def run(self):
        while not self.stop.is_set():
            self.connect()
            self.stop.wait(RETRY_SECONDS)

    def load_state(self):
        """Reads the state file when it changed; True when the state differs from the last read."""
        if not self.state_file:
            return False
        try:
            info = os.stat(self.state_file)
        except OSError:
            return False
        stamp = (info.st_mtime_ns, info.st_size)
        if stamp == self.stamp:
            return False
        try:
            with open(self.state_file, 'rb') as file:
                data = json.loads(file.read().decode('utf-8', 'replace'))
        except (OSError, ValueError):
            return False  # being replaced right now: the next poll reads it
        self.stamp = stamp
        if not isinstance(data, dict) or data == self.state:
            return False
        self.state = data
        return True

    def refresh(self, transport):
        """Between Discord's frames: publishes a changed state (Discord answers with a frame)."""
        now = time.monotonic()
        if now - self.checked < STATE_SECONDS:
            return
        self.checked = now
        if self.load_state():
            transport.write(set_activity(self.pid, activity(self.started, self.state)))

    def connect(self):
        transport = open_transport(self.stop)
        if transport is None:
            return
        self.transport = transport
        published = False
        try:
            handshake(transport, self.client_id, self.stop)
            self.load_state()
            send_activity(transport, activity(self.started, self.state), self.pid, self.stop)
            published = True
            self.checked = time.monotonic()
            while not self.stop.is_set():
                _, data = read_frame(transport, self.stop, timeout=None, idle=lambda: self.refresh(transport))
                if data.get('evt') == 'ERROR':
                    raise ConnectionError('Discord rejected the activity')
        except (OSError, ValueError):
            pass
        finally:
            try:
                if published and self.stop.is_set():
                    transport.write(set_activity(self.pid))
                    transport.write(pack(CLOSE, {}))
            except OSError:
                pass
            finally:
                self.transport = None
                try:
                    transport.close()
                except OSError:
                    pass

    def close(self):
        self.stop.set()
        if self.thread:
            self.thread.join(JOIN_SECONDS)
