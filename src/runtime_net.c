/* libSceNet on host sockets (Winsock / BSD), with the virtual LAN of online play.
 *
 * Every guest socket is a real socket of the host. Addresses are the PS4's (sockaddr with a
 * length byte, port and address in network order, a virtual port for peer-to-peer sockets).
 * Online, the game lives on a virtual LAN (10.77/16, runtime_wire.h): its own address is
 * 10.77.x.y, and so are the other players'. Datagrams to such addresses do not go to the
 * network: they are wrapped in a relay header and sent to the match server's UDP port, which
 * forwards them to the player who owns the destination address. Datagrams that arrive from the
 * relay are handed to the game through a loopback socket per peer, so the game's own socket
 * receives them like any other packet (blocking reads, epoll and select all keep working);
 * sceNetRecvfrom then reports the peer's virtual address instead of the loopback one.
 *
 * Offline, or to any other address, sockets behave as plain host sockets. */
#define _GNU_SOURCE
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "runtime.h"
#include "runtime_wire.h"
#include "runtime_online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SOCKET_ERROR (-1)
#define closesocket close
static int socket_error(void) { return errno; }
#else
static int socket_error(void) { return WSAGetLastError(); }
#endif

/* Guest constants (FreeBSD / PS4). */
#define SCE_AF_INET 2
#define SCE_SOCK_STREAM 1
#define SCE_SOCK_DGRAM 2
#define SCE_SOCK_RAW 3
#define SCE_SOCK_DGRAM_P2P 6
#define SCE_SOCK_STREAM_P2P 10
#define SCE_SOL_SOCKET 0xFFFF
#define SCE_IPPROTO_TCP 6
#define SCE_IPPROTO_UDP 17
#define SCE_SO_REUSEADDR 0x4
#define SCE_SO_KEEPALIVE 0x8
#define SCE_SO_BROADCAST 0x20
#define SCE_SO_LINGER 0x80
#define SCE_SO_REUSEPORT 0x200
#define SCE_SO_SNDBUF 0x1001
#define SCE_SO_RCVBUF 0x1002
#define SCE_SO_SNDLOWAT 0x1003
#define SCE_SO_RCVLOWAT 0x1004
#define SCE_SO_SNDTIMEO 0x1005
#define SCE_SO_RCVTIMEO 0x1006
#define SCE_SO_ERROR 0x1007
#define SCE_SO_TYPE 0x1008
#define SCE_SO_NBIO 0x1200
#define SCE_TCP_NODELAY 1
#define SCE_MSG_PEEK 0x2
#define SCE_MSG_DONTWAIT 0x80
#define SCE_EPOLLIN 0x1
#define SCE_EPOLLOUT 0x2
#define SCE_EPOLLERR 0x8
#define SCE_EPOLLHUP 0x10
#define SCE_EPOLL_CTL_ADD 1
#define SCE_EPOLL_CTL_MOD 2
#define SCE_EPOLL_CTL_DEL 3
/* FreeBSD errno values the guest sees. */
#define E_INTR 4
#define E_BADF 9
#define E_ACCES 13
#define E_FAULT 14
#define E_INVAL 22
#define E_PIPE 32
#define E_AGAIN 35
#define E_INPROGRESS 36
#define E_ALREADY 37
#define E_NOTSOCK 38
#define E_MSGSIZE 40
#define E_PROTONOSUPPORT 43
#define E_OPNOTSUPP 45
#define E_AFNOSUPPORT 47
#define E_ADDRINUSE 48
#define E_ADDRNOTAVAIL 49
#define E_NETDOWN 50
#define E_NETUNREACH 51
#define E_CONNABORTED 53
#define E_CONNRESET 54
#define E_NOBUFS 55
#define E_ISCONN 56
#define E_NOTCONN 57
#define E_SHUTDOWN 58
#define E_TIMEDOUT 60
#define E_CONNREFUSED 61
#define E_HOSTUNREACH 65
#define NET_ERROR(e) ((int32_t)(UINT32_C(0x80410000) | (uint32_t)(e)))
#define RESOLVER_ERROR_NO_HOST ((int32_t)0x80410A0B) /* SCE_NET_RESOLVER_ERROR_... */
#define TRACE(...) runtime_online_log(__VA_ARGS__)

/* SceNetSockaddrIn (16 bytes). */
typedef struct { uint8_t len, family; uint16_t port; uint32_t addr; uint16_t vport; uint8_t zero[6]; } GuestSockaddrIn;
/* SceNetEpollEvent (24 bytes). */
typedef struct { uint32_t events, reserved; uint64_t ident, data; } GuestEpollEvent;

#define MAX_SOCKETS 256
#define SOCKET_ID_BASE 2048 /* ids the file runtime never hands out (its fds are below 1024) */
#define MAX_EPOLLS 32
#define MAX_EPOLL_ITEMS 64
#define MAX_RESOLVERS 16
#define MAX_INJECTORS 64
typedef struct {
    int used, type, nonblock, bound, p2p, connecting, shutdown_done;
    SOCKET fd;
    uint16_t bound_port;    /* host order */
    uint16_t bound_vport;   /* peer-to-peer sockets: the virtual port (host order) */
    uint32_t bound_addr;    /* what the game asked for (host order; 0 = any) */
    char name[32];
} GuestSocket;
typedef struct { int id; uint32_t events; uint64_t data; } EpollItem;
typedef struct { int used; EpollItem items[MAX_EPOLL_ITEMS]; int count; SOCKET wake_send, wake_recv; uint16_t wake_port; char name[32]; } GuestEpoll;
typedef struct { int used; int32_t last_error; char name[32]; } GuestResolver;
typedef struct { int used; uint16_t client, port, vport; SOCKET fd; uint16_t local_port; int64_t last_use; } Injector;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static GuestSocket sockets[MAX_SOCKETS];
static GuestEpoll epolls[MAX_EPOLLS];
static GuestResolver resolvers[MAX_RESOLVERS];
static Injector injectors[MAX_INJECTORS];
static int next_socket = 1, next_epoll = 1, next_resolver = 1, next_pool = 1;
static _Thread_local int32_t net_errno;
static size_t stat_sockets, stat_sent, stat_received, stat_relayed_out, stat_relayed_in, stat_dropped;
/* The relay. */
static struct {
    int configured, running;
    SOCKET fd;
    struct sockaddr_in server;
    uint16_t client_id;
    uint64_t session;
    pthread_t thread;
    uint32_t rtt_us;
    int64_t ping_sent;
} relay;

static int64_t now_us(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000;
}
static void net_init_once(void) {
    static int done;
    if (done) return;
    done = 1;
#ifdef _WIN32
    WSADATA data;
    if (WSAStartup(MAKEWORD(2, 2), &data)) { fputs("STOP: WSAStartup failed\n", stderr); exit(21); }
#endif
}
/* Host socket error -> guest errno. */
static int32_t guest_errno(int e) {
#ifdef _WIN32
    switch (e) {
    case WSAEWOULDBLOCK: return E_AGAIN; case WSAEINPROGRESS: return E_INPROGRESS; case WSAEALREADY: return E_ALREADY;
    case WSAENOTSOCK: return E_NOTSOCK; case WSAEMSGSIZE: return E_MSGSIZE; case WSAEPROTONOSUPPORT: return E_PROTONOSUPPORT;
    case WSAEOPNOTSUPP: return E_OPNOTSUPP; case WSAEAFNOSUPPORT: return E_AFNOSUPPORT; case WSAEADDRINUSE: return E_ADDRINUSE;
    case WSAEADDRNOTAVAIL: return E_ADDRNOTAVAIL; case WSAENETDOWN: return E_NETDOWN; case WSAENETUNREACH: return E_NETUNREACH;
    case WSAECONNABORTED: return E_CONNABORTED; case WSAECONNRESET: return E_CONNRESET; case WSAENOBUFS: return E_NOBUFS;
    case WSAEISCONN: return E_ISCONN; case WSAENOTCONN: return E_NOTCONN; case WSAESHUTDOWN: return E_SHUTDOWN;
    case WSAETIMEDOUT: return E_TIMEDOUT; case WSAECONNREFUSED: return E_CONNREFUSED; case WSAEHOSTUNREACH: return E_HOSTUNREACH;
    case WSAEINTR: return E_INTR; case WSAEBADF: return E_BADF; case WSAEACCES: return E_ACCES; case WSAEFAULT: return E_FAULT;
    case WSAEINVAL: return E_INVAL;
    default: return E_INVAL;
    }
#else
    switch (e) {
    case EAGAIN: return E_AGAIN; case EINPROGRESS: return E_INPROGRESS; case EALREADY: return E_ALREADY;
    case ENOTSOCK: return E_NOTSOCK; case EMSGSIZE: return E_MSGSIZE; case EPROTONOSUPPORT: return E_PROTONOSUPPORT;
    case EOPNOTSUPP: return E_OPNOTSUPP; case EAFNOSUPPORT: return E_AFNOSUPPORT; case EADDRINUSE: return E_ADDRINUSE;
    case EADDRNOTAVAIL: return E_ADDRNOTAVAIL; case ENETDOWN: return E_NETDOWN; case ENETUNREACH: return E_NETUNREACH;
    case ECONNABORTED: return E_CONNABORTED; case ECONNRESET: return E_CONNRESET; case ENOBUFS: return E_NOBUFS;
    case EISCONN: return E_ISCONN; case ENOTCONN: return E_NOTCONN; case ESHUTDOWN: return E_SHUTDOWN;
    case ETIMEDOUT: return E_TIMEDOUT; case ECONNREFUSED: return E_CONNREFUSED; case EHOSTUNREACH: return E_HOSTUNREACH;
    case EINTR: return E_INTR; case EBADF: return E_BADF; case EACCES: return E_ACCES; case EFAULT: return E_FAULT;
    case EPIPE: return E_PIPE; case EINVAL: return E_INVAL;
    default: return E_INVAL;
    }
#endif
}
static int32_t fail(int32_t e) { net_errno = e; return NET_ERROR(e); }
static int32_t fail_host(void) { return fail(guest_errno(socket_error())); }
static void set_nonblocking(SOCKET fd, int on) {
#ifdef _WIN32
    u_long mode = on ? 1 : 0; ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, on ? flags | O_NONBLOCK : flags & ~O_NONBLOCK);
#endif
}
static GuestSocket *socket_of(int id) {
    id -= SOCKET_ID_BASE;
    if (id < 1 || id >= MAX_SOCKETS || !sockets[id].used) return NULL;
    return &sockets[id];
}
static void guest_to_host(const GuestSockaddrIn *in, struct sockaddr_in *out) {
    memset(out, 0, sizeof(*out));
    out->sin_family = AF_INET; out->sin_port = in->port; out->sin_addr.s_addr = in->addr;
}
static void host_to_guest(const struct sockaddr_in *in, GuestSockaddrIn *out) {
    memset(out, 0, sizeof(*out));
    out->len = sizeof(*out); out->family = SCE_AF_INET; out->port = in->sin_port; out->addr = in->sin_addr.s_addr;
}
static void copy_guest_addr(const GuestSockaddrIn *addr, void *out, uint32_t *out_len) {
    if (!out || !out_len) return;
    uint32_t n = *out_len < sizeof(*addr) ? *out_len : sizeof(*addr);
    memcpy(out, addr, n); *out_len = sizeof(*addr);
}
static uint32_t local_virtual_ip(void) { return runtime_online_identity()->virtual_ip; }
static int is_virtual(uint32_t ip_host) { return runtime_online_identity()->online && wire_is_virtual_ip(ip_host); }

/* ---- loopback injectors: one per (peer client, peer port) ---- */
static Injector *injector_for(uint16_t client, uint16_t port, uint16_t vport) {
    Injector *free_slot = NULL, *oldest = NULL;
    for (int i = 0; i < MAX_INJECTORS; ++i) {
        Injector *j = &injectors[i];
        if (j->used && j->client == client && j->port == port && j->vport == vport) return j;
        if (!j->used && !free_slot) free_slot = j;
        if (j->used && (!oldest || j->last_use < oldest->last_use)) oldest = j;
    }
    Injector *j = free_slot;
    if (!j) { j = oldest; closesocket(j->fd); j->used = 0; }
    SOCKET fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd == INVALID_SOCKET) return NULL;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr))) { closesocket(fd); return NULL; }
    socklen_t len = sizeof(addr);
    getsockname(fd, (struct sockaddr *)&addr, &len);
    j->used = 1; j->client = client; j->port = port; j->vport = vport; j->fd = fd; j->local_port = ntohs(addr.sin_port);
    return j;
}
/* A packet that came from 127.0.0.1:<injector port> was relayed: its real source. */
static int injector_source(uint16_t local_port, uint16_t *client, uint16_t *port, uint16_t *vport) {
    for (int i = 0; i < MAX_INJECTORS; ++i)
        if (injectors[i].used && injectors[i].local_port == local_port) {
            *client = injectors[i].client; *port = injectors[i].port; *vport = injectors[i].vport; return 1;
        }
    return 0;
}
static void relay_send_register(void) {
    WireRelayHeader h = {WIRE_RELAY_MAGIC, relay.client_id, 0, 0, 0, 0, 0, WIRE_RELAY_REGISTER, WIRE_VERSION};
    uint8_t packet[WIRE_RELAY_HEADER + 14];
    wire_relay_pack(packet, &h);
    memcpy(packet + WIRE_RELAY_HEADER, &relay.session, 8);
    uint16_t port = runtime_net_p2p_port();
    memcpy(packet + WIRE_RELAY_HEADER + 8, &port, 2);
    memcpy(packet + WIRE_RELAY_HEADER + 10, &relay.rtt_us, 4);
    relay.ping_sent = now_us();
    sendto(relay.fd, (const char *)packet, sizeof(packet), 0, (struct sockaddr *)&relay.server, sizeof(relay.server));
}
static void *relay_thread(void *unused) {
    (void)unused;
    runtime_thread_attach_host("NetRelay");
    uint8_t packet[65536];
    int64_t last_register = 0, last_stats = 0;
    size_t seen_out = 0, seen_in = 0, seen_dropped = 0;
    while (relay.running) {
        int64_t now = now_us();
        if (now - last_register > 2000000) { relay_send_register(); last_register = now; }
        if (now - last_stats > 5000000) { /* every 5 s, when something moved */
            last_stats = now;
            if (stat_relayed_out != seen_out || stat_relayed_in != seen_in || stat_dropped != seen_dropped) {
                TRACE("relay stats: out %zu (+%zu), in %zu (+%zu), dropped %zu, rtt %u us", stat_relayed_out, stat_relayed_out - seen_out,
                      stat_relayed_in, stat_relayed_in - seen_in, stat_dropped, relay.rtt_us);
                seen_out = stat_relayed_out; seen_in = stat_relayed_in; seen_dropped = stat_dropped;
            }
        }
        fd_set set; FD_ZERO(&set); FD_SET(relay.fd, &set);
        struct timeval tv = {0, 500000};
        int ready = select((int)relay.fd + 1, &set, NULL, NULL, &tv);
        if (ready <= 0) continue;
        struct sockaddr_in from; socklen_t from_len = sizeof(from);
        int n = recvfrom(relay.fd, (char *)packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_len);
        if (n < WIRE_RELAY_HEADER) continue;
        WireRelayHeader h;
        if (!wire_relay_unpack(packet, (size_t)n, &h)) continue;
        if (h.kind == WIRE_RELAY_REGISTER) { /* the server's echo: round trip */
            if (relay.ping_sent) relay.rtt_us = (uint32_t)(now_us() - relay.ping_sent);
            continue;
        }
        if (h.kind != WIRE_RELAY_DATA || h.dst_client != relay.client_id) continue;
        /* Find the game's socket bound to the destination port and hand the datagram over. A peer
         * may still address the port the game asked for when another game on this PC holds it:
         * then the game's peer-to-peer socket, whatever its port, is the one meant. */
        pthread_mutex_lock(&lock);
        uint16_t deliver_port = 0, fallback = 0;
        for (int i = 1; i < MAX_SOCKETS; ++i) {
            GuestSocket *s = &sockets[i];
            if (!s->used || !s->bound || (s->type != SCE_SOCK_DGRAM && s->type != SCE_SOCK_DGRAM_P2P)) continue;
            if (s->bound_port == h.dst_port) { deliver_port = s->bound_port; break; }
            if (s->type == SCE_SOCK_DGRAM_P2P && !fallback) fallback = s->bound_port;
        }
        if (!deliver_port) deliver_port = fallback;
        Injector *j = deliver_port ? injector_for(h.src_client, h.src_port, h.src_vport) : NULL;
        if (j) {
            j->last_use = now;
            struct sockaddr_in to; memset(&to, 0, sizeof(to));
            to.sin_family = AF_INET; to.sin_addr.s_addr = htonl(INADDR_LOOPBACK); to.sin_port = htons(deliver_port);
            sendto(j->fd, (const char *)packet + WIRE_RELAY_HEADER, n - WIRE_RELAY_HEADER, 0, (struct sockaddr *)&to, sizeof(to));
            ++stat_relayed_in;
            if (stat_relayed_in <= 8 || stat_relayed_in % 1000 == 0) {
                char label[128];
                snprintf(label, sizeof(label), "relay in #%zu from 10.77.%u.%u:%u to port %u%s", stat_relayed_in,
                         h.src_client >> 8, h.src_client & 255, h.src_port, h.dst_port, deliver_port != h.dst_port ? " (to the game's P2P socket)" : "");
                runtime_online_hex(label, packet + WIRE_RELAY_HEADER, (size_t)(n - WIRE_RELAY_HEADER), 48);
            }
        } else {
            ++stat_dropped;
            if (stat_dropped <= 8) TRACE("relay in: %d bytes for port %u dropped: no socket", n - WIRE_RELAY_HEADER, h.dst_port);
        }
        pthread_mutex_unlock(&lock);
    }
    return NULL;
}
void runtime_net_relay_configure(uint32_t server_ip, uint16_t udp_port, uint16_t client_id, uint64_t session) {
    net_init_once();
    pthread_mutex_lock(&lock);
    memset(&relay.server, 0, sizeof(relay.server));
    relay.server.sin_family = AF_INET; relay.server.sin_addr.s_addr = htonl(server_ip); relay.server.sin_port = htons(udp_port);
    relay.client_id = client_id; relay.session = session;
    if (!relay.configured) {
        relay.fd = socket(AF_INET, SOCK_DGRAM, 0);
        if (relay.fd == INVALID_SOCKET) { pthread_mutex_unlock(&lock); printf("Net: relay socket failed\n"); return; }
        struct sockaddr_in any; memset(&any, 0, sizeof(any)); any.sin_family = AF_INET;
        bind(relay.fd, (struct sockaddr *)&any, sizeof(any));
        relay.configured = 1; relay.running = 1;
        pthread_create(&relay.thread, NULL, relay_thread, NULL);
    }
    pthread_mutex_unlock(&lock);
    relay_send_register();
    printf("Net: relay through %u.%u.%u.%u:%u as 10.77.%u.%u\n", server_ip >> 24, (server_ip >> 16) & 255,
           (server_ip >> 8) & 255, server_ip & 255, udp_port, client_id >> 8, client_id & 255);
}
uint32_t runtime_net_relay_rtt(void) { return relay.rtt_us; }
uint16_t runtime_net_p2p_port(void) {
    uint16_t port = 0;
    for (int i = 1; i < MAX_SOCKETS && !port; ++i)
        if (sockets[i].used && sockets[i].bound && sockets[i].type == SCE_SOCK_DGRAM_P2P) port = sockets[i].bound_port;
    for (int i = 1; i < MAX_SOCKETS && !port; ++i)
        if (sockets[i].used && sockets[i].bound && sockets[i].type == SCE_SOCK_DGRAM) port = sockets[i].bound_port;
    return port;
}
/* Sends a datagram to a virtual address through the relay (lock held). */
static int32_t relay_send(GuestSocket *s, const void *data, size_t size, const GuestSockaddrIn *to) {
    uint32_t dst_ip = ntohl(to->addr);
    if (!s->bound) { /* the OS picks a port when the first datagram goes out: do that now so peers can answer */
        struct sockaddr_in any; memset(&any, 0, sizeof(any)); any.sin_family = AF_INET;
        if (!bind(s->fd, (struct sockaddr *)&any, sizeof(any))) {
            socklen_t len = sizeof(any); getsockname(s->fd, (struct sockaddr *)&any, &len);
            s->bound = 1; s->bound_port = ntohs(any.sin_port);
        }
    }
    if (size > 65536 - WIRE_RELAY_HEADER) return fail(E_MSGSIZE);
    uint16_t dst_client = (uint16_t)(dst_ip & 0xFFFF);
    if (dst_client == relay.client_id || dst_client == runtime_online_identity()->client_id) {
        /* to ourselves: straight into the local socket bound to that port */
        Injector *j = injector_for(dst_client, s->bound_port, s->bound_vport);
        struct sockaddr_in loop; memset(&loop, 0, sizeof(loop));
        loop.sin_family = AF_INET; loop.sin_addr.s_addr = htonl(INADDR_LOOPBACK); loop.sin_port = to->port;
        if (j) sendto(j->fd, (const char *)data, (int)size, 0, (struct sockaddr *)&loop, sizeof(loop));
        return (int32_t)size;
    }
    if (!relay.configured) { ++stat_dropped; return (int32_t)size; } /* not in a room yet: lost like any packet */
    uint8_t *packet = malloc(size + WIRE_RELAY_HEADER);
    if (!packet) return fail(E_NOBUFS);
    WireRelayHeader h = {WIRE_RELAY_MAGIC, relay.client_id, dst_client, s->bound_port, ntohs(to->port), s->bound_vport, ntohs(to->vport), WIRE_RELAY_DATA, WIRE_VERSION};
    wire_relay_pack(packet, &h);
    memcpy(packet + WIRE_RELAY_HEADER, data, size);
    int n = sendto(relay.fd, (const char *)packet, (int)(size + WIRE_RELAY_HEADER), 0, (struct sockaddr *)&relay.server, sizeof(relay.server));
    free(packet);
    if (n < 0) return fail_host();
    ++stat_relayed_out;
    if (stat_relayed_out <= 8 || stat_relayed_out % 1000 == 0) {
        char label[128];
        snprintf(label, sizeof(label), "relay out #%zu from port %u to 10.77.%u.%u:%u", stat_relayed_out, s->bound_port, dst_client >> 8, dst_client & 255, ntohs(to->port));
        runtime_online_hex(label, data, size, 48);
    }
    return (int32_t)size;
}

/* ---- sockets ---- */
static ABI int32_t net_init(void) { net_init_once(); return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name, int32_t size, int32_t flags) { (void)name; (void)size; (void)flags; return next_pool++; }
static ABI int32_t net_pool_destroy(int32_t id) { (void)id; return 0; }
static ABI int32_t net_pool_stats(int32_t id, uint64_t *stats) { (void)id; if (!stats) return fail(E_INVAL); stats[0] = 16384; stats[1] = 0; stats[2] = 0; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int32_t af, const char *src, void *dst) {
    if (af != SCE_AF_INET || !src || !dst) return fail(E_AFNOSUPPORT);
    return inet_pton(AF_INET, src, dst);
}
static ABI const char *net_ntop(int32_t af, const void *src, char *dst, uint32_t size) {
    if (af != SCE_AF_INET || !src || !dst) { net_errno = E_AFNOSUPPORT; return NULL; }
    return inet_ntop(AF_INET, (void *)src, dst, size);
}
static ABI int32_t net_socket(const char *name, int32_t family, int32_t type, int32_t protocol) {
    net_init_once();
    if (!runtime_online_identity()->online) return fail(E_NETDOWN); /* offline: the cable is unplugged, as before */
    if (family != SCE_AF_INET) return fail(E_AFNOSUPPORT);
    int host_type;
    switch (type) {
    case SCE_SOCK_STREAM: case SCE_SOCK_STREAM_P2P: host_type = SOCK_STREAM; break;
    case SCE_SOCK_DGRAM: case SCE_SOCK_DGRAM_P2P: host_type = SOCK_DGRAM; break;
    default: TRACE("sceNetSocket: unsupported type %d", type); return fail(E_PROTONOSUPPORT);
    }
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int tries = 0; tries < MAX_SOCKETS && !id; ++tries) {
        int candidate = next_socket; next_socket = next_socket % (MAX_SOCKETS - 1) + 1;
        if (!sockets[candidate].used) id = candidate;
    }
    if (!id) { pthread_mutex_unlock(&lock); return fail(E_NOBUFS); }
    SOCKET fd = socket(AF_INET, host_type, 0);
    if (fd == INVALID_SOCKET) { int32_t r = fail_host(); pthread_mutex_unlock(&lock); return r; }
    GuestSocket *s = &sockets[id];
    memset(s, 0, sizeof(*s));
    s->used = 1; s->fd = fd; s->type = type; s->p2p = type == SCE_SOCK_DGRAM_P2P || type == SCE_SOCK_STREAM_P2P;
    snprintf(s->name, sizeof(s->name), "%s", name ? name : "");
    ++stat_sockets;
    pthread_mutex_unlock(&lock);
    TRACE("sceNetSocket(%s, type %d, proto %d) -> %d", s->name, type, protocol, SOCKET_ID_BASE + id);
    return SOCKET_ID_BASE + id;
}
static ABI int32_t net_close(int32_t id) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    closesocket(s->fd);
    s->used = 0;
    for (int e = 0; e < MAX_EPOLLS; ++e) {
        GuestEpoll *ep = &epolls[e];
        if (!ep->used) continue;
        for (int i = 0; i < ep->count; ++i) if (ep->items[i].id == id) { ep->items[i] = ep->items[--ep->count]; break; }
    }
    pthread_mutex_unlock(&lock);
    TRACE("sceNetSocketClose(%d)", id);
    return 0;
}
static ABI int32_t net_abort(int32_t id, int32_t flags) {
    (void)flags;
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
#ifdef _WIN32
    shutdown(s->fd, SD_BOTH);
#else
    shutdown(s->fd, SHUT_RDWR);
#endif
    s->shutdown_done = 1;
    pthread_mutex_unlock(&lock);
    TRACE("sceNetSocketAbort(%d)", id);
    return 0;
}
static ABI int32_t net_bind(int32_t id, const GuestSockaddrIn *addr, uint32_t len) {
    if (!addr || len < sizeof(*addr)) return fail(E_INVAL);
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    struct sockaddr_in host; guest_to_host(addr, &host);
    uint32_t wanted = ntohl(addr->addr);
    if (is_virtual(wanted) || wanted == local_virtual_ip()) host.sin_addr.s_addr = htonl(INADDR_ANY);
#ifndef _WIN32
    int one = 1; setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    if (bind(s->fd, (struct sockaddr *)&host, sizeof(host))) {
        /* Another game on this PC holds the port (two players on one machine): a datagram
         * socket takes any free port instead; peers learn the real one through the server. */
        int taken = host.sin_port != 0 && (s->type == SCE_SOCK_DGRAM || s->type == SCE_SOCK_DGRAM_P2P);
        host.sin_port = 0;
        if (!taken || bind(s->fd, (struct sockaddr *)&host, sizeof(host))) { int32_t r = fail_host(); pthread_mutex_unlock(&lock); TRACE("sceNetBind(%d) failed %d", id, net_errno); return r; }
        TRACE("sceNetBind(%d): port %u is in use, another port was taken", id, ntohs(addr->port));
    }
    socklen_t hl = sizeof(host); getsockname(s->fd, (struct sockaddr *)&host, &hl);
    s->bound = 1; s->bound_port = ntohs(host.sin_port); s->bound_addr = wanted; s->bound_vport = ntohs(addr->vport);
    int announce = relay.configured && (s->type == SCE_SOCK_DGRAM_P2P || s->type == SCE_SOCK_DGRAM);
    pthread_mutex_unlock(&lock);
    if (announce) relay_send_register();
    TRACE("sceNetBind(%d, port %u, vport %u)", id, s->bound_port, s->bound_vport);
    return 0;
}
static ABI int32_t net_listen(int32_t id, int32_t backlog) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    int r = listen(s->fd, backlog > 0 ? backlog : 8) ? fail_host() : 0;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t net_connect(int32_t id, const GuestSockaddrIn *addr, uint32_t len) {
    if (!addr || len < sizeof(*addr)) return fail(E_INVAL);
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    SOCKET fd = s->fd; int nonblock = s->nonblock, type = s->type;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in host; guest_to_host(addr, &host);
    if (is_virtual(ntohl(addr->addr)) && (type == SCE_SOCK_STREAM || type == SCE_SOCK_STREAM_P2P)) {
        TRACE("sceNetConnect(%d): TCP to a virtual address is not relayed", id);
        return fail(E_HOSTUNREACH);
    }
    if (connect(fd, (struct sockaddr *)&host, sizeof(host))) {
        int e = socket_error();
#ifdef _WIN32
        if (nonblock && e == WSAEWOULDBLOCK) return fail(E_INPROGRESS);
#else
        if (nonblock && e == EINPROGRESS) return fail(E_INPROGRESS);
#endif
        return fail(guest_errno(e));
    }
    return 0;
}
static ABI int32_t net_accept(int32_t id, GuestSockaddrIn *addr, uint32_t *len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    SOCKET fd = s->fd;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in host; socklen_t hl = sizeof(host);
    SOCKET child = accept(fd, (struct sockaddr *)&host, &hl);
    if (child == INVALID_SOCKET) return fail_host();
    pthread_mutex_lock(&lock);
    int new_id = 0;
    for (int tries = 0; tries < MAX_SOCKETS && !new_id; ++tries) {
        int candidate = next_socket; next_socket = next_socket % (MAX_SOCKETS - 1) + 1;
        if (!sockets[candidate].used) new_id = candidate;
    }
    if (!new_id) { closesocket(child); pthread_mutex_unlock(&lock); return fail(E_NOBUFS); }
    GuestSocket *c = &sockets[new_id]; memset(c, 0, sizeof(*c));
    c->used = 1; c->fd = child; c->type = SCE_SOCK_STREAM; snprintf(c->name, sizeof(c->name), "accepted");
    pthread_mutex_unlock(&lock);
    if (addr && len) { GuestSockaddrIn g; host_to_guest(&host, &g); copy_guest_addr(&g, addr, len); }
    return SOCKET_ID_BASE + new_id;
}
static int host_flags(int32_t flags) {
    int f = 0;
    if (flags & SCE_MSG_PEEK) f |= MSG_PEEK;
#ifndef _WIN32
    if (flags & SCE_MSG_DONTWAIT) f |= MSG_DONTWAIT;
#endif
    return f;
}
static ABI int32_t net_send(int32_t id, const void *data, uint64_t size, int32_t flags) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    SOCKET fd = s->fd;
    pthread_mutex_unlock(&lock);
    int n = send(fd, (const char *)data, (int)(size > 0x7fffffff ? 0x7fffffff : size), host_flags(flags));
    if (n < 0) return fail_host();
    ++stat_sent;
    return n;
}
static ABI int32_t net_sendto(int32_t id, const void *data, uint64_t size, int32_t flags, const GuestSockaddrIn *to, uint32_t to_len) {
    if (!to || to_len < sizeof(*to)) return net_send(id, data, size, flags);
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    if (is_virtual(ntohl(to->addr)) && (s->type == SCE_SOCK_DGRAM || s->type == SCE_SOCK_DGRAM_P2P)) {
        int32_t r = relay_send(s, data, (size_t)size, to);
        pthread_mutex_unlock(&lock);
        return r;
    }
    SOCKET fd = s->fd;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in host; guest_to_host(to, &host);
    int n = sendto(fd, (const char *)data, (int)(size > 0x7fffffff ? 0x7fffffff : size), host_flags(flags), (struct sockaddr *)&host, sizeof(host));
    if (n < 0) return fail_host();
    ++stat_sent;
    return n;
}
static ABI int32_t net_recv(int32_t id, void *data, uint64_t size, int32_t flags) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    SOCKET fd = s->fd;
    pthread_mutex_unlock(&lock);
    int n = recv(fd, (char *)data, (int)(size > 0x7fffffff ? 0x7fffffff : size), host_flags(flags));
    if (n < 0) return fail_host();
    ++stat_received;
    return n;
}
static ABI int32_t net_recvfrom(int32_t id, void *data, uint64_t size, int32_t flags, GuestSockaddrIn *from, uint32_t *from_len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    SOCKET fd = s->fd;
    pthread_mutex_unlock(&lock);
    struct sockaddr_in host; socklen_t hl = sizeof(host); memset(&host, 0, sizeof(host));
    int n = recvfrom(fd, (char *)data, (int)(size > 0x7fffffff ? 0x7fffffff : size), host_flags(flags), (struct sockaddr *)&host, &hl);
    if (n < 0) return fail_host();
    ++stat_received;
    if (from && from_len) {
        GuestSockaddrIn g; host_to_guest(&host, &g);
        if (ntohl(host.sin_addr.s_addr) == INADDR_LOOPBACK) {
            uint16_t client, port, vport;
            pthread_mutex_lock(&lock);
            int relayed = injector_source(ntohs(host.sin_port), &client, &port, &vport);
            pthread_mutex_unlock(&lock);
            if (relayed) { g.addr = htonl(wire_virtual_ip(client)); g.port = htons(port); g.vport = htons(vport); }
        }
        copy_guest_addr(&g, from, from_len);
    }
    return n;
}
static ABI int32_t net_shutdown(int32_t id, int32_t how) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    int r = shutdown(s->fd, how) ? fail_host() : 0;
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t net_setsockopt(int32_t id, int32_t level, int32_t name, const void *value, uint32_t len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    int32_t r = 0;
    int ivalue = 0;
    if (value && len >= 4) memcpy(&ivalue, value, 4);
    if (level == SCE_SOL_SOCKET) {
        switch (name) {
        case SCE_SO_NBIO: s->nonblock = ivalue != 0; set_nonblocking(s->fd, s->nonblock); break;
        case SCE_SO_REUSEADDR: case SCE_SO_REUSEPORT:
#ifndef _WIN32
            setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &ivalue, sizeof(ivalue));
#endif
            break;
        case SCE_SO_KEEPALIVE: setsockopt(s->fd, SOL_SOCKET, SO_KEEPALIVE, (const char *)&ivalue, sizeof(ivalue)); break;
        case SCE_SO_BROADCAST: setsockopt(s->fd, SOL_SOCKET, SO_BROADCAST, (const char *)&ivalue, sizeof(ivalue)); break;
        case SCE_SO_SNDBUF: setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, (const char *)&ivalue, sizeof(ivalue)); break;
        case SCE_SO_RCVBUF: setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, (const char *)&ivalue, sizeof(ivalue)); break;
        case SCE_SO_SNDTIMEO: case SCE_SO_RCVTIMEO: {
            /* SceNetTimeval {sec, usec} of 8 or 16 bytes */
            int64_t sec = 0, usec = 0;
            if (value && len >= 16) { memcpy(&sec, value, 8); memcpy(&usec, (const char *)value + 8, 8); }
            else if (value && len >= 8) { int32_t s32, u32; memcpy(&s32, value, 4); memcpy(&u32, (const char *)value + 4, 4); sec = s32; usec = u32; }
#ifdef _WIN32
            DWORD ms = (DWORD)(sec * 1000 + usec / 1000);
            setsockopt(s->fd, SOL_SOCKET, name == SCE_SO_SNDTIMEO ? SO_SNDTIMEO : SO_RCVTIMEO, (const char *)&ms, sizeof(ms));
#else
            struct timeval tv = {(time_t)sec, (suseconds_t)usec};
            setsockopt(s->fd, SOL_SOCKET, name == SCE_SO_SNDTIMEO ? SO_SNDTIMEO : SO_RCVTIMEO, &tv, sizeof(tv));
#endif
            break;
        }
        case SCE_SO_LINGER: case SCE_SO_SNDLOWAT: case SCE_SO_RCVLOWAT: break;
        default: TRACE("sceNetSetsockopt(%d): SOL_SOCKET option %#x = %d accepted", id, name, ivalue); break;
        }
    } else if (level == SCE_IPPROTO_TCP) {
        if (name == SCE_TCP_NODELAY) setsockopt(s->fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&ivalue, sizeof(ivalue));
    } else TRACE("sceNetSetsockopt(%d): level %d option %#x accepted", id, level, name);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t net_getsockopt(int32_t id, int32_t level, int32_t name, void *value, uint32_t *len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s || !value || !len || *len < 4) { pthread_mutex_unlock(&lock); return fail(s ? E_INVAL : E_BADF); }
    int32_t out = 0;
    if (level == SCE_SOL_SOCKET && name == SCE_SO_ERROR) {
        int e = 0; socklen_t el = sizeof(e);
        getsockopt(s->fd, SOL_SOCKET, SO_ERROR, (char *)&e, &el);
        out = e ? guest_errno(e) : 0;
    } else if (level == SCE_SOL_SOCKET && name == SCE_SO_TYPE) out = s->type;
    else if (level == SCE_SOL_SOCKET && name == SCE_SO_NBIO) out = s->nonblock;
    else if (level == SCE_SOL_SOCKET && (name == SCE_SO_SNDBUF || name == SCE_SO_RCVBUF)) {
        int v = 0; socklen_t vl = sizeof(v);
        getsockopt(s->fd, SOL_SOCKET, name == SCE_SO_SNDBUF ? SO_SNDBUF : SO_RCVBUF, (char *)&v, &vl); out = v;
    }
    memcpy(value, &out, 4); *len = 4;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t net_getsockname(int32_t id, GuestSockaddrIn *addr, uint32_t *len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    struct sockaddr_in host; socklen_t hl = sizeof(host);
    if (getsockname(s->fd, (struct sockaddr *)&host, &hl)) { int32_t r = fail_host(); pthread_mutex_unlock(&lock); return r; }
    GuestSockaddrIn g; host_to_guest(&host, &g);
    if (runtime_online_identity()->online && (host.sin_addr.s_addr == htonl(INADDR_ANY) || s->bound_addr == local_virtual_ip()))
        g.addr = htonl(local_virtual_ip());
    pthread_mutex_unlock(&lock);
    copy_guest_addr(&g, addr, len);
    return 0;
}
static ABI int32_t net_getpeername(int32_t id, GuestSockaddrIn *addr, uint32_t *len) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    if (!s) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    struct sockaddr_in host; socklen_t hl = sizeof(host);
    if (getpeername(s->fd, (struct sockaddr *)&host, &hl)) { int32_t r = fail_host(); pthread_mutex_unlock(&lock); return r; }
    pthread_mutex_unlock(&lock);
    GuestSockaddrIn g; host_to_guest(&host, &g);
    copy_guest_addr(&g, addr, len);
    return 0;
}

/* ---- epoll on select ---- */
static ABI int32_t net_epoll_create(const char *name, int32_t flags) {
    (void)flags;
    net_init_once();
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int tries = 0; tries < MAX_EPOLLS && !id; ++tries) {
        int candidate = next_epoll; next_epoll = next_epoll % (MAX_EPOLLS - 1) + 1;
        if (!epolls[candidate].used) id = candidate;
    }
    if (!id) { pthread_mutex_unlock(&lock); return fail(E_NOBUFS); }
    GuestEpoll *ep = &epolls[id]; memset(ep, 0, sizeof(*ep));
    ep->used = 1; snprintf(ep->name, sizeof(ep->name), "%s", name ? name : "");
    /* A loopback pair wakes a waiting sceNetEpollWait (sceNetEpollAbort). */
    ep->wake_recv = socket(AF_INET, SOCK_DGRAM, 0); ep->wake_send = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (ep->wake_recv != INVALID_SOCKET && !bind(ep->wake_recv, (struct sockaddr *)&a, sizeof(a))) {
        socklen_t al = sizeof(a); getsockname(ep->wake_recv, (struct sockaddr *)&a, &al); ep->wake_port = ntohs(a.sin_port);
        set_nonblocking(ep->wake_recv, 1);
    }
    pthread_mutex_unlock(&lock);
    TRACE("sceNetEpollCreate(%s) -> %d", ep->name, id);
    return id;
}
static ABI int32_t net_epoll_destroy(int32_t id) {
    pthread_mutex_lock(&lock);
    if (id < 1 || id >= MAX_EPOLLS || !epolls[id].used) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    if (epolls[id].wake_recv != INVALID_SOCKET) closesocket(epolls[id].wake_recv);
    if (epolls[id].wake_send != INVALID_SOCKET) closesocket(epolls[id].wake_send);
    epolls[id].used = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t net_epoll_control(int32_t id, int32_t op, int32_t sock, const GuestEpollEvent *event) {
    pthread_mutex_lock(&lock);
    if (id < 1 || id >= MAX_EPOLLS || !epolls[id].used) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    GuestEpoll *ep = &epolls[id];
    int index = -1;
    for (int i = 0; i < ep->count; ++i) if (ep->items[i].id == sock) index = i;
    int32_t r = 0;
    if (op == SCE_EPOLL_CTL_ADD) {
        if (!socket_of(sock) || !event) r = fail(E_BADF);
        else if (index >= 0) r = fail(E_INVAL);
        else if (ep->count >= MAX_EPOLL_ITEMS) r = fail(E_NOBUFS);
        else ep->items[ep->count++] = (EpollItem){sock, event->events, event->data};
    } else if (op == SCE_EPOLL_CTL_MOD) {
        if (index < 0 || !event) r = fail(E_INVAL);
        else { ep->items[index].events = event->events; ep->items[index].data = event->data; }
    } else if (op == SCE_EPOLL_CTL_DEL) {
        if (index < 0) r = fail(E_INVAL);
        else ep->items[index] = ep->items[--ep->count];
    } else r = fail(E_INVAL);
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI int32_t net_epoll_wait(int32_t id, GuestEpollEvent *events, int32_t max, int32_t timeout_us) {
    if (!events || max < 1) return fail(E_INVAL);
    pthread_mutex_lock(&lock);
    if (id < 1 || id >= MAX_EPOLLS || !epolls[id].used) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    GuestEpoll *ep = &epolls[id];
    EpollItem items[MAX_EPOLL_ITEMS]; SOCKET fds[MAX_EPOLL_ITEMS]; int count = 0;
    for (int i = 0; i < ep->count; ++i) {
        GuestSocket *s = socket_of(ep->items[i].id);
        if (!s) continue;
        items[count] = ep->items[i]; fds[count] = s->fd; ++count;
    }
    SOCKET wake = ep->wake_recv;
    pthread_mutex_unlock(&lock);
    fd_set readable, writable, failed;
    FD_ZERO(&readable); FD_ZERO(&writable); FD_ZERO(&failed);
    SOCKET highest = wake;
    if (wake != INVALID_SOCKET) FD_SET(wake, &readable);
    for (int i = 0; i < count; ++i) {
        if (items[i].events & SCE_EPOLLIN) FD_SET(fds[i], &readable);
        if (items[i].events & SCE_EPOLLOUT) FD_SET(fds[i], &writable);
        FD_SET(fds[i], &failed);
        if (fds[i] > highest) highest = fds[i];
    }
    struct timeval tv, *tvp = NULL;
    if (timeout_us >= 0) { tv.tv_sec = timeout_us / 1000000; tv.tv_usec = timeout_us % 1000000; tvp = &tv; }
    int ready = select((int)highest + 1, &readable, &writable, &failed, tvp);
    if (ready < 0) return fail_host();
    if (wake != INVALID_SOCKET && FD_ISSET(wake, &readable)) { char drain[64]; while (recv(wake, drain, sizeof(drain), 0) > 0) {} return fail(E_INTR); }
    int n = 0;
    for (int i = 0; i < count && n < max; ++i) {
        uint32_t got = 0;
        if (FD_ISSET(fds[i], &readable)) got |= SCE_EPOLLIN;
        if (FD_ISSET(fds[i], &writable)) got |= SCE_EPOLLOUT;
        if (FD_ISSET(fds[i], &failed)) got |= SCE_EPOLLERR;
        if (!got) continue;
        events[n].events = got; events[n].reserved = 0; events[n].ident = (uint64_t)items[i].id; events[n].data = items[i].data; ++n;
    }
    return n;
}
static ABI int32_t net_epoll_abort(int32_t id, int32_t flags) {
    (void)flags;
    pthread_mutex_lock(&lock);
    if (id < 1 || id >= MAX_EPOLLS || !epolls[id].used) { pthread_mutex_unlock(&lock); return fail(E_BADF); }
    GuestEpoll *ep = &epolls[id];
    if (ep->wake_send != INVALID_SOCKET && ep->wake_port) {
        struct sockaddr_in a; memset(&a, 0, sizeof(a)); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons(ep->wake_port);
        sendto(ep->wake_send, "w", 1, 0, (struct sockaddr *)&a, sizeof(a));
    }
    pthread_mutex_unlock(&lock);
    return 0;
}

/* ---- resolver ---- */
static ABI int32_t resolver_create(const char *name, int32_t pool, int32_t flags) {
    (void)pool; (void)flags;
    net_init_once();
    pthread_mutex_lock(&lock);
    int id = 0;
    for (int tries = 0; tries < MAX_RESOLVERS && !id; ++tries) {
        int candidate = next_resolver; next_resolver = next_resolver % (MAX_RESOLVERS - 1) + 1;
        if (!resolvers[candidate].used) id = candidate;
    }
    if (id) { resolvers[id].used = 1; resolvers[id].last_error = 0; snprintf(resolvers[id].name, sizeof(resolvers[id].name), "%s", name ? name : ""); }
    pthread_mutex_unlock(&lock);
    return id ? id : fail(E_NOBUFS);
}
static ABI int32_t resolver_destroy(int32_t id) {
    pthread_mutex_lock(&lock);
    if (id >= 1 && id < MAX_RESOLVERS) resolvers[id].used = 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t resolver_ntoa(int32_t id, const char *hostname, uint32_t *addr, int32_t timeout, int32_t retry, int32_t flags) {
    (void)timeout; (void)retry; (void)flags;
    if (!hostname || !addr) return fail(E_INVAL);
    if (inet_pton(AF_INET, hostname, addr) == 1) return 0;
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    int rc = getaddrinfo(hostname, NULL, &hints, &result);
    int32_t r = 0;
    if (rc || !result) r = RESOLVER_ERROR_NO_HOST;
    else *addr = ((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr;
    if (result) freeaddrinfo(result);
    pthread_mutex_lock(&lock);
    if (id >= 1 && id < MAX_RESOLVERS) resolvers[id].last_error = r;
    pthread_mutex_unlock(&lock);
    TRACE("sceNetResolverStartNtoa(%s) -> %s", hostname, r ? "not found" : "ok");
    return r;
}
static ABI int32_t resolver_aton(int32_t id, const uint32_t *addr, char *hostname, int32_t len, int32_t timeout, int32_t retry, int32_t flags) {
    (void)id; (void)timeout; (void)retry; (void)flags;
    if (!addr || !hostname || len < 16) return fail(E_INVAL);
    inet_ntop(AF_INET, (void *)addr, hostname, (size_t)len);
    return 0;
}
static ABI int32_t resolver_error(int32_t id, int32_t *error) {
    if (!error) return fail(E_INVAL);
    pthread_mutex_lock(&lock);
    *error = id >= 1 && id < MAX_RESOLVERS ? resolvers[id].last_error : 0;
    pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t resolver_abort(int32_t id, int32_t flags) { (void)id; (void)flags; return 0; }

/* ---- misc ---- */
static ABI int32_t net_mac(uint8_t *mac, int32_t flags) {
    (void)flags;
    if (!mac) return fail(E_INVAL);
    runtime_online_mac(mac);
    return 0;
}
static ABI int32_t net_ether_ntostr(const uint8_t *mac, char *out, uint64_t size) {
    if (!mac || !out || size < 18) return fail(E_INVAL);
    snprintf(out, (size_t)size, "%02x:%02x:%02x:%02x:%02x:%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return 0;
}
static ABI int32_t net_netstat(void) { return 0; }
static ABI int32_t net_sock_info(int32_t id, void *info, int32_t flags) { (void)id; (void)info; (void)flags; return fail(E_BADF); }

/* ---- the POSIX spellings (libScePosix): the same sockets, -1 and errno on failure ---- */
static int32_t posix_result(int32_t r) {
    if (r >= 0 || (uint32_t)r < 0x80410000u || (uint32_t)r > 0x8041FFFFu) return r;
    *runtime_errno() = net_errno; return -1;
}
static ABI int32_t posix_socket(int32_t family, int32_t type, int32_t protocol) { return posix_result(net_socket("posix", family, type, protocol)); }
static ABI int32_t posix_bind(int32_t id, const GuestSockaddrIn *addr, uint32_t len) { return posix_result(net_bind(id, addr, len)); }
static ABI int32_t posix_connect(int32_t id, const GuestSockaddrIn *addr, uint32_t len) { return posix_result(net_connect(id, addr, len)); }
static ABI int32_t posix_listen(int32_t id, int32_t backlog) { return posix_result(net_listen(id, backlog)); }
static ABI int32_t posix_accept(int32_t id, GuestSockaddrIn *addr, uint32_t *len) { return posix_result(net_accept(id, addr, len)); }
static ABI int32_t posix_send(int32_t id, const void *data, uint64_t size, int32_t flags) { return posix_result(net_send(id, data, size, flags)); }
static ABI int32_t posix_sendto(int32_t id, const void *data, uint64_t size, int32_t flags, const GuestSockaddrIn *to, uint32_t to_len) { return posix_result(net_sendto(id, data, size, flags, to, to_len)); }
static ABI int32_t posix_recv(int32_t id, void *data, uint64_t size, int32_t flags) { return posix_result(net_recv(id, data, size, flags)); }
static ABI int32_t posix_recvfrom(int32_t id, void *data, uint64_t size, int32_t flags, GuestSockaddrIn *from, uint32_t *from_len) { return posix_result(net_recvfrom(id, data, size, flags, from, from_len)); }
static ABI int32_t posix_shutdown(int32_t id, int32_t how) { return posix_result(net_shutdown(id, how)); }
static ABI int32_t posix_setsockopt(int32_t id, int32_t level, int32_t name, const void *value, uint32_t len) { return posix_result(net_setsockopt(id, level, name, value, len)); }
static ABI int32_t posix_getsockopt(int32_t id, int32_t level, int32_t name, void *value, uint32_t *len) { return posix_result(net_getsockopt(id, level, name, value, len)); }
static ABI int32_t posix_getsockname(int32_t id, GuestSockaddrIn *addr, uint32_t *len) { return posix_result(net_getsockname(id, addr, len)); }
static ABI int32_t posix_getpeername(int32_t id, GuestSockaddrIn *addr, uint32_t *len) { return posix_result(net_getpeername(id, addr, len)); }
/* fcntl on a socket: F_GETFL (3) / F_SETFL (4) with O_NONBLOCK (4); files keep their answer (0). */
static ABI int32_t posix_fcntl(int32_t id, int32_t cmd, int64_t arg) {
    pthread_mutex_lock(&lock);
    GuestSocket *s = socket_of(id);
    int32_t r = 0;
    if (s && cmd == 3) r = s->nonblock ? 4 : 0;
    else if (s && cmd == 4) { s->nonblock = (arg & 4) != 0; set_nonblocking(s->fd, s->nonblock); }
    pthread_mutex_unlock(&lock);
    return r;
}
static ABI const char *posix_inet_ntop(int32_t af, const void *src, char *dst, uint32_t size) { return net_ntop(af, src, dst, size); }

void runtime_net_report(void) {
    printf("Runtime: sockets created=%zu, sent=%zu, received=%zu, relayed out=%zu, in=%zu, dropped=%zu, relay rtt=%u us\n",
           stat_sockets, stat_sent, stat_received, stat_relayed_out, stat_relayed_in, stat_dropped, relay.rtt_us);
}
static const RuntimeExport exports[] = {
    {"sceNetInit", net_init}, {"sceNetTerm", net_term}, {"sceNetErrnoLoc", net_errno_loc},
    {"sceNetPoolCreate", net_pool_create}, {"sceNetPoolDestroy", net_pool_destroy}, {"sceNetGetMemoryPoolStats", net_pool_stats},
    {"sceNetHtons", net_htons}, {"sceNetNtohs", net_ntohs}, {"sceNetHtonl", net_htonl}, {"sceNetNtohl", net_ntohl},
    {"sceNetInetPton", net_pton}, {"sceNetInetNtop", net_ntop},
    {"sceNetSocket", net_socket}, {"sceNetSocketClose", net_close}, {"sceNetSocketAbort", net_abort},
    {"sceNetBind", net_bind}, {"sceNetListen", net_listen}, {"sceNetConnect", net_connect}, {"sceNetAccept", net_accept},
    {"sceNetSend", net_send}, {"sceNetSendto", net_sendto}, {"sceNetRecv", net_recv}, {"sceNetRecvfrom", net_recvfrom},
    {"sceNetShutdown", net_shutdown}, {"sceNetSetsockopt", net_setsockopt}, {"sceNetGetsockopt", net_getsockopt},
    {"sceNetGetsockname", net_getsockname}, {"sceNetGetpeername", net_getpeername},
    {"sceNetEpollCreate", net_epoll_create}, {"sceNetEpollDestroy", net_epoll_destroy}, {"sceNetEpollControl", net_epoll_control},
    {"sceNetEpollWait", net_epoll_wait}, {"sceNetEpollAbort", net_epoll_abort},
    {"sceNetResolverCreate", resolver_create}, {"sceNetResolverDestroy", resolver_destroy},
    {"sceNetResolverStartNtoa", resolver_ntoa}, {"sceNetResolverStartAton", resolver_aton},
    {"sceNetResolverGetError", resolver_error}, {"sceNetResolverAbort", resolver_abort},
    {"sceNetGetMacAddress", net_mac}, {"sceNetEtherNtostr", net_ether_ntostr}, {"sceNetShowNetstat", net_netstat},
    {"sceNetGetSockInfo", net_sock_info},
    {"socket", posix_socket}, {"bind", posix_bind}, {"connect", posix_connect}, {"listen", posix_listen}, {"accept", posix_accept},
    {"send", posix_send}, {"sendto", posix_sendto}, {"recv", posix_recv}, {"recvfrom", posix_recvfrom}, {"shutdown", posix_shutdown},
    {"setsockopt", posix_setsockopt}, {"getsockopt", posix_getsockopt}, {"getsockname", posix_getsockname},
    {"getpeername", posix_getpeername}, {"fcntl", posix_fcntl}, {"inet_ntop", posix_inet_ntop},
};
uintptr_t runtime_net_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
