# Online multiplayer on a hosted server

WipEout Omega Collection's online mode runs on PlayStation Network: the game signs in, the
matching service keeps the rooms, a "session" per room feeds the console's invitations, and the
players' consoles exchange race data directly over UDP. This port replaces all of that with a
**match server one of the players hosts**. The game's own code is untouched: its ONLINE menu,
lobbies and races work as on the console, against the hosted server instead of PSN.

## Playing

On the launcher's **Online** page, turn on **Play online**. Turn it off to play offline: the game is
then signed out, with the network cable "unplugged", as before. The **Player name** there is the one
on the Game page: it is your name in the game and online, and two players cannot share one (3 to 16
letters, digits, `-` or `_`; other characters are dropped, and a space becomes `_`). The page shows
the name others will see. Left blank, it is `Player-` and a code derived from the computer's name.

**To host**, choose *Host a game* and press **PLAY**:

1. The launcher asks your router to open the port (UPnP), for TCP and UDP.
2. Your **join code** appears on the Online page, for example `7ZK2-MQ4T-X81A`, already copied.
   Send it to your friends. If Windows asks about the firewall, allow access.
3. In the game: ONLINE, Create Game. The port closes when the game exits.

**To join**, choose *Join a friend*, enter the code and press **PLAY**. In the game: ONLINE, Join
Game. Up to 8 players, as the game allows. Only the host opens a port.

The page says what happened with the router:

| The page says | What it means | What to do |
|---|---|---|
| The router opened the port | Ready | Send the code |
| The router did not open it | UPnP is off or not there | Turn on UPnP, or forward the port (TCP and UDP) by hand |
| Players outside your home cannot reach you | The internet provider (CGNAT) or a second router is in the way, and no port can be opened from here | Ask for a public address, let a friend host, or use a virtual LAN such as Tailscale |
| No internet address found | Offline | Players on your network can still join |

A join code is the host's public address, LAN address and port, packed, with a checksum against
typos. There is no central service: the code **contains your internet address**, like any way of
hosting from home, so send it to people you want to play with. It stops working when your provider
changes your address; hosting again shows the new one. Players on the host's network reach it
through the LAN address in the code, because many routers cannot reach their own public address.

To learn the public address, hosting sends one request to a public STUN server (Cloudflare's, then
Google's). Joining sends nothing to anyone but the host. Mappings on the router last an hour and are
renewed while the game runs, so a crash cannot leave a port open for long.

A server can also run without a game: `bin\wipeout-server.exe --port 7788` (in the package) or
`out/wipeout-server` (from a source build). Forward its port by hand; players enter its address
(`203.0.113.5`, `203.0.113.5:7788` or a host name) where the join code goes.

Without the launcher, the same settings are environment variables for `run.py`:
`WIPEOUT_NET_HOST=1` (host, `WIPEOUT_NET_PORT` for another port), `WIPEOUT_NET_SERVER=host:port` (join),
`WIPEOUT_USER_NAME` (player name). `WIPEOUT_NET_TRACE=1` prints every network and PSN call to the log.
With neither variable set, nothing changes: the game plays offline. `run.py` itself opens no port.

## How it works

Everything lives in the runtime (`src/`), the layer that replaces the PS4's system libraries:

| Part | File | What it does |
|---|---|---|
| Account and network status | `runtime_np.c` | NpManager and NetCtl: one local account signed in (online ID = player name, account ID = a hash of it), PlayStation Plus and availability checks pass, the console's address is its address on the virtual LAN below. |
| Sockets | `runtime_net.c` | libSceNet (and the POSIX spellings) on Winsock / BSD sockets: TCP, UDP, the PS4's peer-to-peer datagram sockets with virtual ports, epoll, the resolver. |
| Virtual LAN and relay | `runtime_net.c` | Online, the game sees a 10.77.0.0/16 LAN where every player has an address. Datagrams to such addresses are wrapped and sent to the server's UDP port, which forwards them to the owner; arriving datagrams are handed to the game's own socket through loopback so blocking reads and epoll keep working. Players never need an open port. |
| Matching | `runtime_matching.c` | libSceNpMatching2 as the game's toolkit (`libSceNpToolkit2.prx`, the game's own code) uses it: contexts, worlds, rooms (create, join, search, leave, room and member data, kick, messages), signaling (peer addresses, round trips) and every room event, over one TCP connection to the server. The toolkit's callbacks run on the connection's reader thread, as the console's library does. |
| PSN web API | `runtime_webapi.c` | The toolkit binds every room to a PSN "session" through the sessionInvitation web API before it creates the room. That API is answered locally with plausible JSON; nothing leaves the PC. |
| Join codes and router | `scripts/online_host.py` | The launcher's side: join codes, UPnP port mappings and their renewal, the public address (STUN), telling why a host cannot be reached. |
| Match server | `runtime_matchserver.c` | Rooms, members, attributes, search filters, events, ownership hand-over, the UDP relay. In the game with `WIPEOUT_NET_HOST=1`, or on its own as `wipeout-server` (`tools/wipeout_server.c`). |
| Wire protocol | `runtime_wire.c/.h` | Framed little-endian messages between the runtime and the server, and the relay header. |

The game's race traffic is RakNet over one peer-to-peer UDP socket (port 3658, virtual port
9846). Peers' addresses come from the matching signaling: the runtime hands out virtual addresses,
so whatever the game tells other players about itself stays reachable through the relay. When two
games share one PC, the second cannot bind port 3658; it takes another port, tells the server, and
the relay delivers to the game's peer-to-peer socket regardless of the port a peer used.

RakNet derives each peer's GUID from the console's Ethernet address, and treats a peer with its own
GUID as "self". The runtime therefore gives every player a different address, derived from the
Player name. This is why two players cannot share a name: the race start and all race data go
over these GUIDs, and with equal ones nothing would leave the PC.

Trace lines (`WIPEOUT_NET_TRACE=1`) start with `Net:`, `Matching:` and `Server:`.

## What is not there

- Leaderboards, statistics and the "community" ribbon (Sony's REACH/OTG web services): their
  HTTP requests fail as before and the game goes on without them.
- Invitations through the console's UI, friends, profiles, trophies online: not provided; the
  toolkit's requests for them answer "not found" or "signed out".
- Voice chat.
- NAT traversal between players: by design everything goes through the server.
- The sixteen "worlds" the server reports are all alike; the game picks one from its location
  setting (US = world 2). Rooms are only found by players in the same world.

## Troubleshooting

- *The ONLINE menu reports an error at once*: the server could not be reached. The host's game has
  to be running. Check that the code is the host's latest, and what their Online page says about
  the router.
- *A player sees the room but others never see them in the lobby*: UDP does not get through. A
  port forwarded by hand must be forwarded for UDP too.
- *The ONLINE menu reports an error, and the log says "name already in use"*: a player on another
  machine has the same Player name. Two players behind one router look like one machine: with
  the same name, the second replaces the first.
- *A player stays on "STARTING IN 00:00:00" while the host waits for them*: the RakNet connection
  between the two did not come up. Send both logs with `WIPEOUT_NET_TRACE=1`.
- Problems reports need `user\last_run.log` of both the host and a player, with `WIPEOUT_NET_TRACE=1`
  (launcher: Advanced > Log shows the file).

## Reference: what the toolkit expects

These layouts were taken from the toolkit's code (`sce_module/libSceNpToolkit2.prx`) and are what
`runtime_matching.c` produces. Offsets are bytes; callbacks use the SysV ABI.

Callbacks: request `cb(u16 ctx, u32 request, u16 event, int error, const void *data, void *arg)`;
room event `cb(u16 ctx, u64 room, u16 event, const void *data, void *arg)`; room message
`cb(u16 ctx, u64 room, u16 from, u16 event, const RoomMessageInfo *data, void *arg)`; signaling
`cb(u16 ctx, u64 room, u16 peer, u16 event, int error, void *arg)`; context
`cb(u16 ctx, u16 event, u8 cause, int error, void *arg)`. Context events: 0x6F02 start, 0x6F03 stop
(0x6F01 start over). Room events: 0x8101 member joined, 0x8102 member left, 0x1103 kicked out,
0x1104 room destroyed, 0x1105 owner updated, 0x1106 room data updated, 0x8107 member data
updated. Signaling: 0x5101 dead, 0x5102 established. Messages: 0x9101 chat, 0x9102 data.

Member record (0x58): next, join date, account ID, platform (3 = PS4) at 0x18, online ID at 0x20
(16 + terminator + 3), member ID at 0x38, team, NAT type, flags at 0x3C (bit 31 = owner), member
attributes (array, 0x20 each: update date, id at 8, data at 0x10, size at 0x18) at 0x48 and their
count at 0x50. Room record given to create/join/get-room-data callbacks (0x28): room header,
member list head, member count, me, owner. Room header: open slots at 2, max slots at 8, server
ID at 0xA, world ID at 0xC, lobby ID at 0x10, room ID at 0x18, flags at 0x40, room attributes
(array, 0x28 each: update date, member at 8, id at 0x10, data at 0x18, size at 0x20) at 0x48 and
count at 0x50. Search results are a list of external room records (0xA8) with the owner's account
and online ID at 0x40/0x50 and the searchable attributes at 0x78 onward. The toolkit copies 256
bytes out of every attribute whatever its declared size.

Create-room request: max slots at 0, flags at 8, world at 0xC, password at 0x18, slot mask at
0x20, then (pointer, count) pairs at 0x60 (room internal attributes 0x57/0x58), 0x70 (searchable
integers 0x4C..0x53), 0x80 (searchable binary 0x54), 0x90 (external 0x55/0x56), 0xA0 (member
attribute 0x59) and the signaling option at 0xB0. Join: room ID, password, group label, member
attributes and count. Search: option, world, lobby, range, flag filter and value, integer filters
(operator at 0, attribute at 8), binary filters, attribute IDs. Requests carry an option block
(callback, argument, timeout in microseconds, application request ID).
