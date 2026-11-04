/* The match server: rooms for online play, hosted by a player (WIPEOUT_NET_HOST=1 starts it inside
 * the game) or run on its own (wipeout-server). One thread, one TCP port for the matching protocol
 * (runtime_wire.h) and the same UDP port for the relay of the players' game packets.
 *
 * Model: one server (id 1) with one world (id 1); rooms are created by the first player and
 * live while a member remains. Every change in a room goes to all its members as events; the
 * players' packets to each other travel through the relay, so nobody but the host needs an
 * open port. */
#define _GNU_SOURCE
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "runtime_wire.h"
#include "runtime_matchserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <time.h>
#include <pthread.h>
#ifndef _WIN32
#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif

#define PLATFORM_PS4 3
#define UDP_TIMEOUT_US (30 * 1000000)

typedef struct {
    int used;
    SOCKET sock;
    uint16_t client_id, p2p_port, member_id;
    uint64_t session, account_id, room_id;
    uint32_t platform, rtt_us;
    char online_id[WIRE_ONLINE_ID + 1];
    struct sockaddr_in udp; int udp_known; int64_t last_udp;
    uint8_t *in; size_t in_size, in_capacity;
    int hello_done;
    uint32_t peer_ip;          /* network order: tells a restart from another player */
    WireBinList user_info;
} Client;
typedef struct { int used; WireRoom room; uint16_t next_member; int64_t created; } Room;

static struct {
    int running, quiet;
    SOCKET listener, udp;
    uint16_t port;
    Client clients[WIRE_MAX_CLIENTS];
    Room rooms[WIRE_MAX_ROOMS];
    uint64_t next_room;
    pthread_t thread;
    pthread_mutex_t lock;
    char motd[128];
    size_t stat_connections, stat_rooms, stat_relayed;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER, .next_room = 1000};

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static uint64_t rtc_now(void) { return (uint64_t)time(NULL) * 1000000 + UINT64_C(62135596800000000); }
static void logline(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logline(const char *fmt, ...) {
    if (S.quiet) return;
    va_list args; va_start(args, fmt);
    fputs("Server: ", stdout); vprintf(fmt, args); fputc('\n', stdout); fflush(stdout);
    va_end(args);
}
static uint64_t random64(void) {
    static uint64_t x;
    if (!x) x = (uint64_t)time(NULL) ^ ((uint64_t)(uintptr_t)&x << 17) ^ (uint64_t)now_us();
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    return x ? x : 1;
}
static Client *client_by_id(uint16_t id) {
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) if (S.clients[i].used && S.clients[i].client_id == id) return &S.clients[i];
    return NULL;
}
static Room *room_by_id(uint64_t id) {
    for (int i = 0; i < WIRE_MAX_ROOMS; ++i) if (S.rooms[i].used && S.rooms[i].room.room_id == id) return &S.rooms[i];
    return NULL;
}
static int member_index(const WireRoom *r, uint16_t member_id) {
    for (uint16_t i = 0; i < r->member_count; ++i) if (r->members[i].member_id == member_id) return i;
    return -1;
}
static Client *client_of_member(const WireRoom *r, uint16_t member_id) {
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i)
        if (S.clients[i].used && S.clients[i].room_id == r->room_id && S.clients[i].member_id == member_id) return &S.clients[i];
    return NULL;
}

/* ---- sending ---- */
static void send_buf(Client *c, WireBuf *b) {
    wire_finish(b);
    if (!b->error && c->sock != INVALID_SOCKET) {
        const uint8_t *data = b->data; size_t size = b->size;
        while (size) {
            int n = send(c->sock, (const char *)data, (int)(size > 65536 ? 65536 : size), 0);
            if (n <= 0) break;
            data += n; size -= (size_t)n;
        }
    }
    wire_free(b);
}
static void send_ok(Client *c, uint32_t token) { WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_OK, token); send_buf(c, &b); }
static void send_error(Client *c, uint32_t token, int32_t code, const char *text) {
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_ERROR, token);
    wire_put_u32(&b, (uint32_t)code); wire_put_string(&b, text); send_buf(c, &b);
}
static void send_room(Client *c, uint32_t token, const Room *r) {
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_ROOM, token);
    WireRoom copy = r->room; copy.me_id = c->member_id;
    wire_put_room(&b, &copy); send_buf(c, &b);
}
static void send_member_event(Room *r, uint16_t type, const WireMember *m, uint8_t cause, Client *except) {
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        Client *c = client_of_member(&r->room, r->room.members[i].member_id);
        if (!c || c == except) continue;
        WireBuf b; wire_writer_init(&b); wire_begin(&b, type, 0);
        wire_put_u64(&b, r->room.room_id); wire_put_member(&b, m); wire_put_u8(&b, cause); wire_put_blob(&b, NULL, 0);
        send_buf(c, &b);
    }
}
static void send_signaling(Client *to, const Room *r, const WireMember *peer, int established) {
    Client *pc = client_by_id(peer->client_id);
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_SIGNALING, 0);
    wire_put_u64(&b, r->room.room_id); wire_put_u16(&b, peer->member_id); wire_put_u8(&b, (uint8_t)established);
    wire_put_u16(&b, peer->client_id); wire_put_u16(&b, pc ? pc->p2p_port : peer->p2p_port);
    wire_put_u32(&b, (pc ? pc->rtt_us : 0) + to->rtt_us);
    send_buf(to, &b);
}
static void refresh_flags(Room *r) {
    if (r->room.member_count >= r->room.max_slot) r->room.flag_attr |= WIRE_FLAG_FULL; else r->room.flag_attr &= ~WIRE_FLAG_FULL;
    r->room.joined_slot_mask = 0;
    for (uint16_t i = 0; i < r->room.member_count; ++i) r->room.joined_slot_mask |= UINT64_C(1) << (i & 63);
}
static void send_room_updated(Room *r, uint32_t prev_flags) {
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        Client *c = client_of_member(&r->room, r->room.members[i].member_id);
        if (!c) continue;
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_ROOM_UPDATED, 0);
        wire_put_u64(&b, r->room.room_id); wire_put_u32(&b, prev_flags);
        WireRoom copy = r->room; copy.me_id = c->member_id;
        wire_put_room(&b, &copy); send_buf(c, &b);
    }
}
static void destroy_room(Room *r, uint8_t cause) {
    logline("room %llu destroyed", (unsigned long long)r->room.room_id);
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        Client *c = client_of_member(&r->room, r->room.members[i].member_id);
        if (!c) continue;
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_DESTROYED, 0);
        wire_put_u64(&b, r->room.room_id); wire_put_u8(&b, cause); wire_put_blob(&b, NULL, 0); send_buf(c, &b);
        c->room_id = 0; c->member_id = 0;
    }
    r->used = 0;
}
/* A member leaves (left, kicked, disconnected): events, ownership, the room's end. */
static void remove_member(Room *r, uint16_t member_id, uint8_t cause, int kicked) {
    int j = member_index(&r->room, member_id);
    if (j < 0) return;
    WireMember gone = r->room.members[j];
    Client *leaver = client_of_member(&r->room, member_id);
    if (kicked && leaver) {
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_KICKED, 0);
        wire_put_u64(&b, r->room.room_id); wire_put_u8(&b, cause); wire_put_blob(&b, NULL, 0); send_buf(leaver, &b);
    }
    if (leaver) { leaver->room_id = 0; leaver->member_id = 0; }
    for (int k = j; k + 1 < r->room.member_count; ++k) r->room.members[k] = r->room.members[k + 1];
    --r->room.member_count;
    logline("%s left room %llu (%u left)", gone.online_id, (unsigned long long)r->room.room_id, r->room.member_count);
    if (!r->room.member_count) { r->used = 0; logline("room %llu is empty and gone", (unsigned long long)r->room.room_id); return; }
    refresh_flags(r);
    send_member_event(r, WIRE_EV_LEFT, &gone, cause, NULL);
    if (r->room.owner_id == member_id) {
        if (!(r->room.flag_attr & WIRE_FLAG_OWNER_AUTO_GRANT)) { destroy_room(r, WIRE_CAUSE_LEAVE_ACTION); return; }
        /* the longest-standing member takes over */
        int best = 0;
        for (uint16_t i = 1; i < r->room.member_count; ++i) if (r->room.members[i].join_date < r->room.members[best].join_date) best = i;
        uint16_t prev = r->room.owner_id, next = r->room.members[best].member_id;
        r->room.owner_id = next;
        for (uint16_t i = 0; i < r->room.member_count; ++i) r->room.members[i].flag_attr = r->room.members[i].member_id == next ? WIRE_MEMBER_FLAG_OWNER : 0;
        for (uint16_t i = 0; i < r->room.member_count; ++i) {
            Client *c = client_of_member(&r->room, r->room.members[i].member_id);
            if (!c) continue;
            WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_OWNER, 0);
            wire_put_u64(&b, r->room.room_id); wire_put_u16(&b, prev); wire_put_u16(&b, next); wire_put_u8(&b, WIRE_CAUSE_LEAVE_ACTION);
            wire_put_u8(&b, r->room.has_password); wire_put_u64(&b, r->room.password); wire_put_blob(&b, NULL, 0);
            send_buf(c, &b);
        }
        logline("room %llu: %s is the owner now", (unsigned long long)r->room.room_id, r->room.members[best].online_id);
    }
}
static void drop_client(Client *c, uint8_t cause) {
    if (c->room_id) { Room *r = room_by_id(c->room_id); if (r) remove_member(r, c->member_id, cause, 0); }
    if (c->sock != INVALID_SOCKET) closesocket(c->sock);
    if (c->hello_done) logline("%s disconnected", c->online_id);
    free(c->in);
    memset(c, 0, sizeof(*c));
    c->sock = INVALID_SOCKET;
}

/* ---- requests ---- */
static void handle_hello(Client *c, uint32_t token, WireBuf *in) {
    uint32_t version = wire_get_u32(in);
    uint64_t account = wire_get_u64(in);
    char online_id[WIRE_ONLINE_ID + 1]; wire_get_string(in, online_id, sizeof(online_id));
    uint32_t platform = wire_get_u32(in); uint16_t wanted = wire_get_u16(in), p2p = wire_get_u16(in);
    if (in->error || version != WIRE_VERSION) { send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "protocol version mismatch"); return; }
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) {
        Client *o = &S.clients[i];
        if (o != c && o->used && o->hello_done && o->account_id == account) {
            if (o->peer_ip != c->peer_ip) { /* another machine: another player with this name */
                logline("%s refused: the name is in use", online_id);
                send_error(c, token, WIRE_ERR_NAME_IN_USE, "name already in use");
                return;
            }
            /* the same machine again (a restart): the old connection is stale */
            logline("%s connected again; the earlier connection is dropped", o->online_id);
            drop_client(o, WIRE_CAUSE_MEMBER_DISAPPEARED);
        }
    }
    uint16_t id = wanted;
    if (!id || id == 0xFFFF || client_by_id(id)) {
        id = 0;
        for (uint16_t candidate = 1; candidate < 0xFFFF && !id; ++candidate) if (!client_by_id(candidate)) id = candidate;
    }
    c->client_id = id; c->session = random64(); c->account_id = account; c->platform = platform ? platform : PLATFORM_PS4;
    c->p2p_port = p2p; snprintf(c->online_id, sizeof(c->online_id), "%s", online_id); c->hello_done = 1;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_WELCOME, token);
    wire_put_u32(&b, WIRE_VERSION); wire_put_u16(&b, id); wire_put_u64(&b, c->session); wire_put_u16(&b, S.port);
    wire_put_u16(&b, WIRE_SERVER_ID); wire_put_u32(&b, WIRE_WORLD_ID); wire_put_string(&b, S.motd);
    send_buf(c, &b);
    logline("%s connected (address 10.77.%u.%u)", c->online_id, id >> 8, id & 255);
}
/* Worlds 1..WIRE_WORLD_COUNT all exist: the game picks one by its location setting (its
 * numbering is the PSN server's), and rooms live in the world they were created in. */
static void handle_get_worlds(Client *c, uint32_t token) {
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_WORLDS, token);
    wire_put_u32(&b, WIRE_WORLD_COUNT);
    for (uint32_t world = 1; world <= WIRE_WORLD_COUNT; ++world) {
        uint32_t rooms = 0, members = 0;
        for (int i = 0; i < WIRE_MAX_ROOMS; ++i)
            if (S.rooms[i].used && S.rooms[i].room.world_id == world) { ++rooms; members += S.rooms[i].room.member_count; }
        wire_put_u32(&b, world); wire_put_u32(&b, rooms); wire_put_u32(&b, members);
    }
    send_buf(c, &b);
}
static void fill_member(Client *c, Room *r, WireMember *m, const WireBinList *attrs) {
    memset(m, 0, sizeof(*m));
    m->member_id = r->next_member++;
    if (!r->next_member) r->next_member = 1;
    m->client_id = c->client_id; m->p2p_port = c->p2p_port; m->nat_type = 2; m->platform = c->platform;
    m->account_id = c->account_id; m->join_date = rtc_now(); m->rtt_us = c->rtt_us;
    snprintf(m->online_id, sizeof(m->online_id), "%s", c->online_id);
    m->attrs = *attrs;
}
static void handle_create_room(Client *c, uint32_t token, WireBuf *in) {
    if (c->room_id) { send_error(c, token, WIRE_ERR_ALREADY_JOINED, "already in a room"); return; }
    Room *r = NULL;
    for (int i = 0; i < WIRE_MAX_ROOMS; ++i) if (!S.rooms[i].used) { r = &S.rooms[i]; break; }
    if (!r) { send_error(c, token, WIRE_ERR_SERVER_UNAVAILABLE, "no room left"); return; }
    memset(r, 0, sizeof(*r));
    WireRoom *room = &r->room;
    room->world_id = wire_get_u32(in); room->max_slot = wire_get_u32(in); room->flag_attr = wire_get_u32(in); room->lobby_id = wire_get_u64(in);
    room->has_password = wire_get_u8(in); room->password = wire_get_u64(in); room->password_slot_mask = wire_get_u64(in);
    room->sig_type = wire_get_u8(in); room->sig_flag = wire_get_u8(in); room->sig_hub = wire_get_u16(in);
    wire_get_bin_list(in, &room->bin_internal); wire_get_bin_list(in, &room->bin_external);
    wire_get_bin_list(in, &room->searchable_bin); wire_get_int_list(in, &room->searchable_int);
    WireBinList member_attrs; wire_get_bin_list(in, &member_attrs);
    if (in->error) { send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "bad create request"); return; }
    if (room->max_slot < 2) room->max_slot = 2;
    if (room->max_slot > WIRE_MAX_MEMBERS) room->max_slot = WIRE_MAX_MEMBERS;
    if (!room->world_id) room->world_id = WIRE_WORLD_ID;
    room->server_id = WIRE_SERVER_ID; room->room_id = S.next_room++;
    r->used = 1; r->next_member = 1; r->created = now_us();
    fill_member(c, r, &room->members[0], &member_attrs);
    room->members[0].flag_attr = WIRE_MEMBER_FLAG_OWNER;
    room->member_count = 1; room->owner_id = room->members[0].member_id;
    c->room_id = room->room_id; c->member_id = room->owner_id;
    refresh_flags(r);
    ++S.stat_rooms;
    logline("%s created room %llu (world %u, %u slots, flags %#x, %s)", c->online_id, (unsigned long long)room->room_id,
            room->world_id, room->max_slot, room->flag_attr, room->has_password ? "password" : "open");
    send_room(c, token, r);
}
static void handle_join_room(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint8_t has_password = wire_get_u8(in); uint64_t password = wire_get_u64(in);
    WireBinList attrs; wire_get_bin_list(in, &attrs);
    if (in->error) { send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "bad join request"); return; }
    if (c->room_id) { send_error(c, token, WIRE_ERR_ALREADY_JOINED, "already in a room"); return; }
    Room *r = room_by_id(room_id);
    if (!r) { send_error(c, token, WIRE_ERR_NO_SUCH_ROOM, "no such room"); return; }
    if (r->room.member_count >= r->room.max_slot || r->room.member_count >= WIRE_MAX_MEMBERS) { send_error(c, token, WIRE_ERR_ROOM_FULL, "room full"); return; }
    if (r->room.flag_attr & WIRE_FLAG_CLOSED) { send_error(c, token, WIRE_ERR_ROOM_FULL, "room closed"); return; }
    if (r->room.has_password && (!has_password || password != r->room.password)) { send_error(c, token, WIRE_ERR_PASSWORD_MISMATCH, "wrong password"); return; }
    WireMember *m = &r->room.members[r->room.member_count++];
    fill_member(c, r, m, &attrs);
    c->room_id = room_id; c->member_id = m->member_id;
    refresh_flags(r);
    logline("%s joined room %llu (%u of %u)", c->online_id, (unsigned long long)room_id, r->room.member_count, r->room.max_slot);
    send_room(c, token, r);
    send_member_event(r, WIRE_EV_JOINED, m, WIRE_CAUSE_LEAVE_ACTION, c);
    /* signaling: the newcomer and every member reach each other through the relay at once */
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        WireMember *peer = &r->room.members[i];
        if (peer->member_id == m->member_id) continue;
        Client *pc = client_of_member(&r->room, peer->member_id);
        send_signaling(c, r, peer, 1);
        if (pc) send_signaling(pc, r, m, 1);
    }
}
static void handle_leave_room(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in);
    Room *r = room_by_id(room_id);
    if (!r || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    uint16_t member_id = c->member_id;
    send_ok(c, token);
    remove_member(r, member_id, WIRE_CAUSE_LEAVE_ACTION, 0);
}
static void send_room_list(Client *c, uint32_t token, Room **list, uint32_t count, uint32_t start, uint32_t total) {
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_ROOM_LIST, token);
    wire_put_u32(&b, start); wire_put_u32(&b, total); wire_put_u32(&b, count);
    for (uint32_t i = 0; i < count; ++i) { WireRoom copy = list[i]->room; copy.me_id = 0; wire_put_room(&b, &copy); }
    send_buf(c, &b);
}
static void handle_search(Client *c, uint32_t token, WireBuf *in) {
    WireSearch s; wire_get_search(in, &s);
    if (in->error) { send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "bad search"); return; }
    Room *matches[WIRE_MAX_ROOMS]; uint32_t total = 0;
    for (int i = 0; i < WIRE_MAX_ROOMS; ++i) if (S.rooms[i].used && wire_room_matches(&S.rooms[i].room, &s)) matches[total++] = &S.rooms[i];
    uint32_t start = s.start_index < total ? s.start_index : total;
    uint32_t max = s.max ? s.max : 20;
    uint32_t count = total - start < max ? total - start : max;
    if (count > 32) count = 32;
    logline("%s searched: %u of %u rooms", c->online_id, count, total);
    send_room_list(c, token, matches + start, count, start, total);
}
static void handle_get_external(Client *c, uint32_t token, WireBuf *in) {
    uint16_t n = wire_get_u16(in);
    Room *list[16]; uint32_t count = 0;
    for (uint16_t i = 0; i < n && i < 16; ++i) { Room *r = room_by_id(wire_get_u64(in)); if (r) list[count++] = r; }
    send_room_list(c, token, list, count, 0, count);
}
static void handle_get_internal(Client *c, uint32_t token, WireBuf *in) {
    Room *r = room_by_id(wire_get_u64(in));
    if (!r) { send_error(c, token, WIRE_ERR_NO_SUCH_ROOM, "no such room"); return; }
    send_room(c, token, r);
}
static void handle_get_member(Client *c, uint32_t token, WireBuf *in) {
    Room *r = room_by_id(wire_get_u64(in)); uint16_t member_id = wire_get_u16(in);
    int j = r ? member_index(&r->room, member_id) : -1;
    if (j < 0) { send_error(c, token, WIRE_ERR_NO_SUCH_MEMBER, "no such member"); return; }
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_MEMBER, token);
    wire_put_member(&b, &r->room.members[j]); send_buf(c, &b);
}
static void handle_set_room_internal(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint32_t filter = wire_get_u32(in), flags = wire_get_u32(in);
    WireBinList attrs; wire_get_bin_list(in, &attrs);
    uint8_t has_mask = wire_get_u8(in); uint64_t mask = wire_get_u64(in);
    Room *r = room_by_id(room_id);
    if (in->error || !r || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    uint32_t prev = r->room.flag_attr;
    r->room.flag_attr = (r->room.flag_attr & ~filter) | (flags & filter);
    for (uint16_t i = 0; i < attrs.count; ++i) wire_bin_set(&r->room.bin_internal, attrs.items[i].id, attrs.items[i].data, attrs.items[i].size);
    if (has_mask) r->room.password_slot_mask = mask;
    refresh_flags(r);
    send_ok(c, token);
    send_room_updated(r, prev);
}
static void handle_set_room_external(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in);
    WireIntList sint; WireBinList sbin, bin;
    wire_get_int_list(in, &sint); wire_get_bin_list(in, &sbin); wire_get_bin_list(in, &bin);
    Room *r = room_by_id(room_id);
    if (in->error || !r || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    for (uint16_t i = 0; i < sint.count; ++i) wire_int_set(&r->room.searchable_int, sint.items[i].id, sint.items[i].value);
    for (uint16_t i = 0; i < sbin.count; ++i) wire_bin_set(&r->room.searchable_bin, sbin.items[i].id, sbin.items[i].data, sbin.items[i].size);
    for (uint16_t i = 0; i < bin.count; ++i) wire_bin_set(&r->room.bin_external, bin.items[i].id, bin.items[i].data, bin.items[i].size);
    send_ok(c, token);
}
static void handle_set_member(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint16_t member_id = wire_get_u16(in); uint8_t team = wire_get_u8(in);
    WireBinList attrs; wire_get_bin_list(in, &attrs);
    Room *r = room_by_id(room_id);
    int j = r ? member_index(&r->room, member_id) : -1;
    if (in->error || j < 0 || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NO_SUCH_MEMBER, "no such member"); return; }
    if (member_id != c->member_id && r->room.owner_id != c->member_id) { send_error(c, token, WIRE_ERR_NOT_OWNER, "not the owner"); return; }
    WireMember *m = &r->room.members[j];
    m->team_id = team;
    for (uint16_t i = 0; i < attrs.count; ++i) wire_bin_set(&m->attrs, attrs.items[i].id, attrs.items[i].data, attrs.items[i].size);
    send_ok(c, token);
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        Client *o = client_of_member(&r->room, r->room.members[i].member_id);
        if (!o) continue;
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_MEMBER_UPDATED, 0);
        wire_put_u64(&b, room_id); wire_put_member(&b, m); send_buf(o, &b);
    }
}
static void handle_set_sigopt(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint8_t type = wire_get_u8(in), flag = wire_get_u8(in); uint16_t hub = wire_get_u16(in);
    Room *r = room_by_id(room_id);
    if (!r || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    r->room.sig_type = type; r->room.sig_flag = flag; r->room.sig_hub = hub;
    send_ok(c, token);
}
static void handle_kick(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint16_t member_id = wire_get_u16(in);
    Room *r = room_by_id(room_id);
    if (!r || c->room_id != room_id) { send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    if (r->room.owner_id != c->member_id) { send_error(c, token, WIRE_ERR_NOT_OWNER, "not the owner"); return; }
    if (member_index(&r->room, member_id) < 0) { send_error(c, token, WIRE_ERR_NO_SUCH_MEMBER, "no such member"); return; }
    send_ok(c, token);
    logline("%s kicked member %u from room %llu", c->online_id, member_id, (unsigned long long)room_id);
    remove_member(r, member_id, WIRE_CAUSE_KICKOUT_ACTION, 1);
}
static void handle_message(Client *c, uint32_t token, WireBuf *in) {
    uint64_t room_id = wire_get_u64(in); uint8_t cast = wire_get_u8(in), chat = wire_get_u8(in);
    uint16_t n = wire_get_u16(in); uint16_t targets[WIRE_MAX_MEMBERS]; uint16_t count = 0;
    for (uint16_t i = 0; i < n; ++i) { uint16_t t = wire_get_u16(in); if (count < WIRE_MAX_MEMBERS) targets[count++] = t; }
    uint8_t *msg = malloc(WIRE_MAX_MESSAGE);
    uint32_t size = msg ? wire_get_blob(in, msg, WIRE_MAX_MESSAGE) : 0;
    Room *r = room_by_id(room_id);
    if (in->error || !msg || !r || c->room_id != room_id) { free(msg); send_error(c, token, WIRE_ERR_NOT_IN_ROOM, "not in that room"); return; }
    send_ok(c, token);
    for (uint16_t i = 0; i < r->room.member_count; ++i) {
        const WireMember *m = &r->room.members[i];
        if (m->member_id == c->member_id) continue;
        int wanted = cast == WIRE_CAST_BROADCAST;
        if (cast == WIRE_CAST_UNICAST || cast == WIRE_CAST_MULTICAST) for (uint16_t k = 0; k < count; ++k) if (targets[k] == m->member_id) wanted = 1;
        if (cast == WIRE_CAST_MULTICAST_TEAM && count && m->team_id == (uint8_t)targets[0]) wanted = 1;
        if (!wanted) continue;
        Client *o = client_of_member(&r->room, m->member_id);
        if (!o) continue;
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_EV_MESSAGE, 0);
        wire_put_u64(&b, room_id); wire_put_u16(&b, c->member_id); wire_put_u8(&b, cast); wire_put_u8(&b, chat);
        wire_put_blob(&b, msg, size); send_buf(o, &b);
    }
    free(msg);
}
static void handle_ping(Client *c, uint32_t token, WireBuf *in) {
    (void)wire_get_u64(in);
    uint32_t rtt = wire_get_u32(in);
    if (rtt) c->rtt_us = rtt;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_PONG, token);
    wire_put_u32(&b, c->rtt_us ? c->rtt_us : 1000); send_buf(c, &b);
}
static void handle_set_user_info(Client *c, uint32_t token, WireBuf *in) {
    WireBinList attrs; wire_get_bin_list(in, &attrs);
    if (!in->error) c->user_info = attrs;
    send_ok(c, token);
}
static void handle_frame(Client *c, uint16_t type, uint32_t token, const uint8_t *payload, size_t size) {
    WireBuf in; wire_reader_init(&in, payload, size);
    if (!c->hello_done && type != WIRE_HELLO) { send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "hello first"); return; }
    switch (type) {
    case WIRE_HELLO: handle_hello(c, token, &in); break;
    case WIRE_GET_WORLDS: handle_get_worlds(c, token); break;
    case WIRE_CREATE_ROOM: handle_create_room(c, token, &in); break;
    case WIRE_JOIN_ROOM: handle_join_room(c, token, &in); break;
    case WIRE_LEAVE_ROOM: handle_leave_room(c, token, &in); break;
    case WIRE_SEARCH: handle_search(c, token, &in); break;
    case WIRE_GET_ROOM_EXTERNAL: handle_get_external(c, token, &in); break;
    case WIRE_GET_ROOM_INTERNAL: handle_get_internal(c, token, &in); break;
    case WIRE_GET_MEMBER: handle_get_member(c, token, &in); break;
    case WIRE_SET_ROOM_INTERNAL: handle_set_room_internal(c, token, &in); break;
    case WIRE_SET_ROOM_EXTERNAL: handle_set_room_external(c, token, &in); break;
    case WIRE_SET_MEMBER: handle_set_member(c, token, &in); break;
    case WIRE_SET_SIGOPT: handle_set_sigopt(c, token, &in); break;
    case WIRE_KICK: handle_kick(c, token, &in); break;
    case WIRE_MESSAGE: handle_message(c, token, &in); break;
    case WIRE_PING: handle_ping(c, token, &in); break;
    case WIRE_SET_USER_INFO: handle_set_user_info(c, token, &in); break;
    case WIRE_P2P_PORT: c->p2p_port = wire_get_u16(&in); send_ok(c, token); break;
    default: send_error(c, token, WIRE_ERR_INVALID_ARGUMENT, "unknown request"); break;
    }
}
/* Bytes arrived on a client's connection: complete frames are handled. */
static int feed_client(Client *c, const uint8_t *data, size_t size) {
    if (c->in_size + size > c->in_capacity) {
        size_t capacity = c->in_capacity ? c->in_capacity * 2 : 4096;
        while (capacity < c->in_size + size) capacity *= 2;
        if (capacity > WIRE_MAX_FRAME + WIRE_HEADER + 65536) return -1;
        uint8_t *in = realloc(c->in, capacity);
        if (!in) return -1;
        c->in = in; c->in_capacity = capacity;
    }
    memcpy(c->in + c->in_size, data, size); c->in_size += size;
    for (;;) {
        if (c->in_size < WIRE_HEADER) return 0;
        uint16_t type; uint32_t token;
        int64_t length = wire_parse_header(c->in, &type, &token);
        if (length < 0) return -1;
        if (c->in_size < WIRE_HEADER + (size_t)length) return 0;
        handle_frame(c, type, token, c->in + WIRE_HEADER, (size_t)length);
        if (!c->used) return -1; /* dropped itself */
        size_t consumed = WIRE_HEADER + (size_t)length;
        memmove(c->in, c->in + consumed, c->in_size - consumed); c->in_size -= consumed;
    }
}

/* ---- the UDP relay ---- */
static void handle_udp(void) {
    uint8_t packet[65536];
    struct sockaddr_in from; socklen_t from_len = sizeof(from);
    int n = recvfrom(S.udp, (char *)packet, sizeof(packet), 0, (struct sockaddr *)&from, &from_len);
    if (n < WIRE_RELAY_HEADER) return;
    WireRelayHeader h;
    if (!wire_relay_unpack(packet, (size_t)n, &h)) return;
    Client *src = client_by_id(h.src_client);
    if (!src) return;
    if (h.kind == WIRE_RELAY_REGISTER) {
        if (n < WIRE_RELAY_HEADER + 10) return;
        uint64_t session; memcpy(&session, packet + WIRE_RELAY_HEADER, 8);
        if (session != src->session) return;
        uint16_t port; memcpy(&port, packet + WIRE_RELAY_HEADER + 8, 2);
        int port_changed = port && port != src->p2p_port;
        if (port) src->p2p_port = port;
        if (n >= WIRE_RELAY_HEADER + 14) { uint32_t rtt; memcpy(&rtt, packet + WIRE_RELAY_HEADER + 10, 4); if (rtt) src->rtt_us = rtt; }
        src->udp = from; src->udp_known = 1; src->last_udp = now_us();
        sendto(S.udp, (const char *)packet, n, 0, (struct sockaddr *)&from, sizeof(from)); /* echo: the client measures its round trip */
        if (port_changed && src->room_id) {
            /* the game's port is known (or changed) after the join: the others learn it through a
             * fresh signaling event, which makes their toolkit ask for the connection info again */
            Room *r = room_by_id(src->room_id);
            int j = r ? member_index(&r->room, src->member_id) : -1;
            if (j >= 0) {
                r->room.members[j].p2p_port = port;
                for (uint16_t i = 0; i < r->room.member_count; ++i) {
                    if (r->room.members[i].member_id == src->member_id) continue;
                    Client *o = client_of_member(&r->room, r->room.members[i].member_id);
                    if (o) send_signaling(o, r, &r->room.members[j], 1);
                }
                logline("%s plays from port %u", src->online_id, port);
            }
        }
        return;
    }
    if (h.kind != WIRE_RELAY_DATA) return;
    if (!src->udp_known || src->udp.sin_addr.s_addr != from.sin_addr.s_addr || src->udp.sin_port != from.sin_port) return;
    Client *dst = client_by_id(h.dst_client);
    if (!dst || !dst->udp_known) return;
    sendto(S.udp, (const char *)packet, n, 0, (struct sockaddr *)&dst->udp, sizeof(dst->udp));
    ++S.stat_relayed;
}

/* ---- the loop ---- */
static void *server_thread(void *unused) {
    (void)unused;
    while (S.running) {
        fd_set readable; FD_ZERO(&readable);
        FD_SET(S.listener, &readable); FD_SET(S.udp, &readable);
        SOCKET highest = S.listener > S.udp ? S.listener : S.udp;
        for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) if (S.clients[i].used) { FD_SET(S.clients[i].sock, &readable); if (S.clients[i].sock > highest) highest = S.clients[i].sock; }
        struct timeval tv = {0, 200000};
        int ready = select((int)highest + 1, &readable, NULL, NULL, &tv);
        if (ready < 0) continue;
        pthread_mutex_lock(&S.lock);
        if (FD_ISSET(S.listener, &readable)) {
            struct sockaddr_in from; socklen_t from_len = sizeof(from);
            SOCKET s = accept(S.listener, (struct sockaddr *)&from, &from_len);
            if (s != INVALID_SOCKET) {
                Client *c = NULL;
                for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) if (!S.clients[i].used) { c = &S.clients[i]; break; }
                if (!c) closesocket(s);
                else {
                    memset(c, 0, sizeof(*c)); c->used = 1; c->sock = s; c->peer_ip = from.sin_addr.s_addr;
                    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
#ifdef _WIN32
                    DWORD timeout = 3000; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char *)&timeout, sizeof(timeout));
#else
                    struct timeval timeout = {3, 0}; setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
#endif
                    ++S.stat_connections;
                }
            }
        }
        if (FD_ISSET(S.udp, &readable)) handle_udp();
        for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) {
            Client *c = &S.clients[i];
            if (!c->used || !FD_ISSET(c->sock, &readable)) continue;
            uint8_t data[16384];
            int n = recv(c->sock, (char *)data, sizeof(data), 0);
            if (n <= 0 || feed_client(c, data, (size_t)n) < 0) { if (c->used) drop_client(c, WIRE_CAUSE_MEMBER_DISAPPEARED); }
        }
        pthread_mutex_unlock(&S.lock);
    }
    return NULL;
}
static int listen_on(uint16_t port) {
#ifdef _WIN32
    WSADATA data; WSAStartup(MAKEWORD(2, 2), &data);
#endif
    S.listener = socket(AF_INET, SOCK_STREAM, 0);
    S.udp = socket(AF_INET, SOCK_DGRAM, 0);
    if (S.listener == INVALID_SOCKET || S.udp == INVALID_SOCKET) return -1;
    int one = 1;
#ifndef _WIN32
    setsockopt(S.listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#endif
    (void)one;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_ANY); addr.sin_port = htons(port);
    if (bind(S.listener, (struct sockaddr *)&addr, sizeof(addr)) || listen(S.listener, 16)) return -1;
    if (bind(S.udp, (struct sockaddr *)&addr, sizeof(addr))) return -1;
    S.port = port;
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) S.clients[i].sock = INVALID_SOCKET;
    return 0;
}
int runtime_matchserver_start(uint16_t port) {
    if (S.running) return 0;
    if (!port) port = WIRE_DEFAULT_PORT;
    if (listen_on(port)) { printf("Server: cannot listen on port %u\n", port); return -1; }
    snprintf(S.motd, sizeof(S.motd), "WipEout PC match server");
    S.running = 1;
    if (pthread_create(&S.thread, NULL, server_thread, NULL)) { S.running = 0; return -1; }
    printf("Server: listening on TCP and UDP port %u\n", port);
    return 0;
}
void runtime_matchserver_stop(void) {
    if (!S.running) return;
    S.running = 0;
    pthread_join(S.thread, NULL);
    pthread_mutex_lock(&S.lock);
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) if (S.clients[i].used) drop_client(&S.clients[i], WIRE_CAUSE_SERVER_OPERATION);
    closesocket(S.listener); closesocket(S.udp);
    pthread_mutex_unlock(&S.lock);
}
void runtime_matchserver_set_quiet(int quiet) { S.quiet = quiet; }
void runtime_matchserver_stats(size_t *connections, size_t *rooms, size_t *relayed, unsigned *online, unsigned *open_rooms) {
    pthread_mutex_lock(&S.lock);
    if (connections) *connections = S.stat_connections;
    if (rooms) *rooms = S.stat_rooms;
    if (relayed) *relayed = S.stat_relayed;
    unsigned n = 0, r = 0;
    for (int i = 0; i < WIRE_MAX_CLIENTS; ++i) if (S.clients[i].used && S.clients[i].hello_done) ++n;
    for (int i = 0; i < WIRE_MAX_ROOMS; ++i) if (S.rooms[i].used) ++r;
    if (online) *online = n;
    if (open_rooms) *open_rooms = r;
    pthread_mutex_unlock(&S.lock);
}
