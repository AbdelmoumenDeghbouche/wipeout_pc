/* Online play: the pieces of the runtime that talk to the match server (runtime_np.c,
 * runtime_net.c, runtime_matching.c) and to each other. */
#ifndef WIPEOUT_RUNTIME_ONLINE_H
#define WIPEOUT_RUNTIME_ONLINE_H
#include <stdint.h>
#include <stddef.h>

/* The local player's identity on the private network. */
typedef struct {
    int online;                 /* WIPEOUT_NET_SERVER or WIPEOUT_NET_HOST set: signed in, connected */
    char online_id[17];         /* 3..16 characters */
    uint64_t account_id;
    uint16_t client_id;         /* this console's address on the virtual LAN: 10.77.<id>>8>.<id&255> */
    uint32_t virtual_ip;        /* host byte order */
} OnlineIdentity;
const OnlineIdentity *runtime_online_identity(void);
/* The match server assigned another address (the wanted one was taken). */
void runtime_online_set_client_id(uint16_t client_id);
/* SceNpId (36 bytes) of the local player. */
void runtime_online_np_id(void *np_id_36);
/* The local Ethernet address: differs per player (the game's RakNet GUID comes from it). */
void runtime_online_mac(uint8_t mac[6]);
/* WIPEOUT_NET_TRACE=1: every network and PSN call is printed ("Net: ..."). */
int runtime_online_trace(void);
void runtime_online_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* "<label>: <size> bytes: xx xx ..." (the first `max` bytes), when tracing. */
void runtime_online_hex(const char *label, const void *data, size_t size, size_t max);
/* The whole buffer, 32 bytes per line with offsets, when tracing. */
void runtime_online_dump(const char *label, const void *data, size_t size);

/* PSN account and NetCtl (runtime_np.c). */
uintptr_t runtime_np_resolve(const char *name);

/* The PSN web API answered locally (runtime_webapi.c). */
uintptr_t runtime_webapi_resolve(const char *name);

/* Sockets (runtime_net.c). */
uintptr_t runtime_net_resolve(const char *name);
void runtime_net_report(void);
/* The relay: datagrams to virtual addresses travel through the match server's UDP port.
 * Called by the matching client once the server welcomed it (server address in host order). */
void runtime_net_relay_configure(uint32_t server_ip, uint16_t udp_port, uint16_t client_id, uint64_t session);
/* The UDP port of the game's peer-to-peer socket (what peers should send to), 0 if none yet. */
uint16_t runtime_net_p2p_port(void);
/* Round trip to the match server measured by the relay keepalives, microseconds (0: unknown). */
uint32_t runtime_net_relay_rtt(void);

/* Matching (runtime_matching.c). */
uintptr_t runtime_matching_resolve(const char *name);
void runtime_matching_report(void);
/* Host side (runtime_matchserver.c): an in-process match server, as wipeout-server runs one. */
int runtime_matchserver_start(uint16_t port);
void runtime_matchserver_stop(void);
#endif
