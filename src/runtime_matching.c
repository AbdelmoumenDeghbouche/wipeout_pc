/* libSceNpMatching2 for WipEout's NpToolkit2: rooms on a self-hosted match server.
 *
 * The toolkit (the game's own libSceNpToolkit2.prx) drives this API; the game never calls it
 * directly. What the toolkit passes and reads was taken from its code: docs/online-multiplayer.md
 * lists every structure. Each request goes to the match server (runtime_wire.h) over the
 * context's TCP connection; the answer, and every room event the server sends, come back as
 * the toolkit's callbacks. The callbacks run on the context's reader thread, never on the
 * caller's: the toolkit holds its own lock while it waits for them, as on the console.
 *
 * Callback data lives in one allocation per callback (an arena), freed when the callback
 * returns: the toolkit copies what it keeps. Local queries (member list, signaling state,
 * connection info) answer from the context's copy of its rooms, kept current by the events. */
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
#define closesocket close
#endif

#define TRACE(...) runtime_online_log(__VA_ARGS__)
/* Error codes (SCE_NP_MATCHING2_ERROR_*, SCE_NP_MATCHING2_SERVER_ERROR_*). */
#define M2_OK 0
#define M2_ERROR_NOT_INITIALIZED ((int32_t)0x80550C02)
#define M2_ERROR_INVALID_ARGUMENT ((int32_t)0x80550C03)
#define M2_ERROR_CONTEXT_NOT_FOUND ((int32_t)0x80550C06)
#define M2_ERROR_CONTEXT_ALREADY_STARTED ((int32_t)0x80550C0A)
#define M2_ERROR_CONTEXT_NOT_STARTED ((int32_t)0x80550C0B)
#define M2_ERROR_CONTEXT_MAX ((int32_t)0x80550C0C)
#define M2_ERROR_ROOM_NOT_FOUND ((int32_t)0x80550C0D)
#define M2_ERROR_MEMBER_NOT_FOUND ((int32_t)0x80550C0E)
#define M2_ERROR_TIMEOUT ((int32_t)0x80550C13)
#define M2_ERROR_CONNECTION ((int32_t)0x80550D01)
#define M2_ERROR_NO_MEMORY ((int32_t)0x80550C01)
/* Events. */
#define EV_CONTEXT_START_OVER 0x6F01
#define EV_CONTEXT_START 0x6F02
#define EV_CONTEXT_STOP 0x6F03
#define EV_ROOM_MEMBER_JOINED 0x8101
#define EV_ROOM_MEMBER_LEFT 0x8102
#define EV_ROOM_KICKED_OUT 0x1103
#define EV_ROOM_DESTROYED 0x1104
#define EV_ROOM_OWNER_UPDATED 0x1105
#define EV_ROOM_DATA_INTERNAL_UPDATED 0x1106
#define EV_ROOM_MEMBER_DATA_INTERNAL_UPDATED 0x8107
#define EV_SIGNALING_DEAD 0x5101
#define EV_SIGNALING_ESTABLISHED 0x5102
#define EV_ROOM_MSG_CHAT 0x9101
#define EV_ROOM_MSG_MESSAGE 0x9102
#define EV_REQUEST 0x0101 /* request callbacks: the toolkit does not look at the event id */
#define SIG_NONE 0
#define SIG_ESTABLISHED 1
#define SIG_DEAD 2
#define PLATFORM_PS4 3

/* Guest callbacks (sysv ABI). */
typedef ABI void (*ContextCallback)(uint16_t ctx, uint16_t event, uint8_t cause, int32_t error, void *arg);
typedef ABI void (*RequestCallback)(uint16_t ctx, uint32_t req, uint16_t event, int32_t error, const void *data, void *arg);
typedef ABI void (*RoomEventCallback)(uint16_t ctx, uint64_t room, uint16_t event, const void *data, void *arg);
typedef ABI void (*RoomMessageCallback)(uint16_t ctx, uint64_t room, uint16_t src, uint16_t event, const void *data, void *arg);
typedef ABI void (*SignalingCallback)(uint16_t ctx, uint64_t room, uint16_t peer, uint16_t event, int32_t error, void *arg);

/* Guest structures (packed exactly as the toolkit reads them). */
typedef struct { uint16_t id; uint8_t pad[6]; const void *data; uint64_t size; } GBinAttr;                       /* 0x18 */
typedef struct { uint16_t id; uint16_t pad; uint32_t value; } GIntAttr;                                        /* 8 */
typedef struct { uint64_t update_date; uint16_t id; uint8_t pad[6]; const void *data; uint64_t size; } GMemberBinAttr;  /* 0x20 */
typedef struct { uint64_t update_date; uint16_t update_member; uint8_t pad0[6]; uint16_t id; uint8_t pad1[6]; const void *data; uint64_t size; } GRoomBinAttr; /* 0x28 */
typedef struct { uint8_t type, flag; uint16_t hub; uint8_t reserved[4]; } GSigOptParam;                         /* 8 */
typedef struct { uint8_t data[16]; uint64_t length; } GOptData;                                                /* 0x18 */
typedef struct GMember {
    struct GMember *next; uint64_t join_date, account_id; uint32_t platform; uint32_t pad0;
    char online_id[16]; char term; char dummy[3]; uint32_t pad1;
    uint16_t member_id; uint8_t team_id, nat_type; uint32_t flag_attr;
    void *room_group; GMemberBinAttr *attrs; uint64_t attr_count;
} GMember;                                                                                                       /* 0x58 */
typedef struct {
    uint8_t public_slots, private_slots; uint16_t open_slots; uint32_t cur_members;
    uint16_t max_slot, server_id; uint32_t world_id; uint64_t lobby_id, room_id;
    uint64_t password_slot_mask, joined_slot_mask; uint64_t reserved[2];
    uint32_t flag_attr; uint32_t pad; GRoomBinAttr *attrs; uint64_t attr_count;
    void *room_group; uint64_t room_group_count;
} GRoomInternal;                                                                                                 /* 0x68 */
typedef struct { GRoomInternal *room; GMember *members; uint64_t member_count; GMember *me, *owner; } GRoomDataInternal; /* 0x28 */
typedef struct GRoomExternal {
    struct GRoomExternal *next; uint16_t max_slot, cur_members; uint32_t flag_attr;
    uint16_t server_id; uint16_t pad0; uint32_t world_id; uint64_t lobby_id, room_id;
    uint64_t password_slot_mask, joined_slot_mask;
    uint8_t public_slots, private_slots, open_public, open_private; uint32_t pad1;
    uint64_t owner_account; uint32_t owner_platform; uint32_t pad2;
    char owner_online_id[16]; char owner_term; char owner_dummy[3]; uint32_t pad3;
    void *room_group; uint64_t room_group_count;
    GIntAttr *int_attrs; uint64_t int_attr_count; GBinAttr *sbin_attrs; uint64_t sbin_attr_count;
    GBinAttr *bin_attrs; uint64_t bin_attr_count;
} GRoomExternal;                                                                                                 /* 0xA8 */
typedef struct GWorld { struct GWorld *next; uint32_t world_id, lobbies, max_lobby_members, cur_lobby_members, cur_rooms, cur_room_members; } GWorld; /* 0x20 */
typedef struct { GWorld *worlds; uint64_t count; } GWorldList;
typedef struct { uint32_t start, total, size, pad; GRoomExternal *rooms; } GSearchResponse;
typedef struct { GRoomExternal *rooms; } GExternalListResponse;
typedef struct { GMember *member; } GMemberResponse;
typedef struct { uint16_t server_id; uint16_t pad; uint32_t world_id; uint64_t room_id; uint32_t rtt; uint32_t pad2; } GPingResponse;
typedef struct { GMember *member; uint8_t cause; uint8_t pad[7]; GOptData opt; } GMemberUpdateInfo;              /* 0x28 */
typedef struct { uint8_t cause; uint8_t pad[3]; int32_t error; GOptData opt; } GRoomUpdateInfo;                /* 0x20 */
typedef struct { uint16_t prev_owner, new_owner; uint8_t cause; uint8_t pad[3]; const uint64_t *password; GOptData opt; } GOwnerUpdateInfo; /* 0x28 */
typedef struct { GRoomInternal *room; uint32_t *new_flags, *prev_flags; void *slot_info, *prev_group, *reserved[2]; GRoomBinAttr **attrs; uint64_t attr_count; } GRoomDataUpdateInfo; /* 0x48 */
typedef struct { GMember *member; uint32_t *new_flags, *prev_flags; uint8_t *new_team; GMemberBinAttr **attrs; uint64_t attr_count; } GMemberDataUpdateInfo; /* 0x30 */
typedef struct { uint8_t filtered, cast_type; uint8_t pad[6]; uint8_t dst[0x30]; const void *msg; uint64_t msg_len; } GRoomMessageInfo; /* 0x48 */

/* Guest request structures. */
typedef struct { RequestCallback cb; void *arg; uint32_t timeout; uint16_t app_req_id; uint8_t pad[2]; } GRequestOptParam;
typedef struct {
    uint16_t head; uint8_t pad0[6]; uint32_t flag_attr, world_id; uint64_t lobby_id;
    const uint64_t *password; const uint64_t *password_slot_mask; uint64_t reserved[7];
    const GBinAttr *bin_internal; uint64_t bin_internal_count; const GIntAttr *sint; uint64_t sint_count;
    const GBinAttr *sbin; uint64_t sbin_count; const GBinAttr *bin_external; uint64_t bin_external_count;
    const GBinAttr *member_bin; uint64_t member_bin_count; const GSigOptParam *sig_opt;
} GCreateJoinRoomRequest;
typedef struct { uint64_t room_id; const uint64_t *password; const void *group_label; const GBinAttr *member_bin; uint64_t member_bin_count; } GJoinRoomRequest;
typedef struct { uint8_t op; uint8_t pad[7]; GIntAttr attr; } GIntFilter;
typedef struct { uint8_t op; uint8_t pad[7]; GBinAttr attr; } GBinFilter;
typedef struct {
    uint32_t option, world_id; uint64_t lobby_id; uint32_t start_index, max; uint32_t flag_filter, flag_attr;
    const GIntFilter *int_filters; uint64_t int_filter_count; const GBinFilter *bin_filters; uint64_t bin_filter_count;
    const uint16_t *attr_ids; uint64_t attr_id_count;
} GSearchRoomRequest;
typedef struct { uint64_t room_id; GOptData opt; } GLeaveRoomRequest;
typedef struct { uint64_t room_id; uint32_t flag_filter, flag_attr; const GBinAttr *attrs; uint64_t attr_count; const void *password_config; const void *reserved; const uint64_t *password_slot_mask; } GSetRoomInternalRequest;
typedef struct { uint64_t room_id; const GIntAttr *sint; uint64_t sint_count; const GBinAttr *sbin; uint64_t sbin_count; const GBinAttr *bin; uint64_t bin_count; } GSetRoomExternalRequest;
typedef struct { uint64_t room_id; uint16_t member_id; uint8_t team_id; uint8_t pad[5]; uint64_t reserved; const GBinAttr *attrs; uint64_t attr_count; } GSetMemberRequest;
typedef struct { uint64_t room_id; GSigOptParam sig_opt; } GSetSigOptRequest;
typedef struct { uint64_t room_id; uint16_t member_id; uint8_t block; uint8_t pad[5]; GOptData opt; } GKickoutRequest;
typedef struct { uint64_t room_id; uint8_t cast_type; uint8_t pad[7]; union { uint16_t unicast; struct { const uint16_t *ids; uint64_t count; } multicast; uint8_t team; } dst; const void *msg; uint64_t msg_len; } GSendMessageRequest;
typedef struct { uint64_t room_id; uint16_t member_id; uint8_t pad[6]; const uint16_t *attr_ids; uint64_t attr_id_count; } GGetMemberRequest;
typedef struct { uint64_t room_id; const uint16_t *attr_ids; uint64_t attr_id_count; } GGetRoomInternalRequest;
typedef struct { const uint64_t *room_ids; uint64_t room_id_count; const uint16_t *attr_ids; uint64_t attr_id_count; } GGetRoomExternalListRequest;
typedef struct { uint64_t room_id; } GPingRequest;
typedef struct { uint16_t server_id; uint8_t pad[6]; const GBinAttr *attrs; uint64_t attr_count; } GSetUserInfoRequest;
typedef struct { uint16_t server_id; uint8_t pad[6]; const uint64_t *accounts; uint64_t account_count; const uint16_t *attr_ids; uint64_t attr_id_count; } GGetUserInfoRequest;

/* ---- arena for callback data ---- */
typedef struct { uint8_t *data; size_t size, used; } Arena;
#define ATTR_BUFFER 256 /* the toolkit copies this much out of every attribute, whatever its size */
static size_t attr_buffer(size_t size) { return size < ATTR_BUFFER ? ATTR_BUFFER : size; }
static void *arena_alloc(Arena *a, size_t size) {
    size = (size + 15) & ~(size_t)15;
    if (a->used + size > a->size) return NULL;
    void *p = a->data + a->used; a->used += size; memset(p, 0, size); return p;
}
static Arena *arena_new(size_t size) {
    Arena *a = calloc(1, sizeof(*a) + size);
    if (!a) return NULL;
    a->data = (uint8_t *)(a + 1); a->size = size; return a;
}
#define ARENA_ROOM (sizeof(GRoomDataInternal) + sizeof(GRoomInternal) + 64 * 1024 + WIRE_MAX_MEMBERS * (sizeof(GMember) + WIRE_MAX_ATTRS * (sizeof(GMemberBinAttr) + WIRE_MAX_ATTR_SIZE + ATTR_BUFFER)))

/* ---- state ---- */
#define MAX_CONTEXTS 8
#define MAX_ROOMS 4
#define MAX_PENDING 32
typedef struct { uint8_t status; uint32_t rtt_us; } SignalingState;
typedef struct { int used; WireRoom room; SignalingState sig[WIRE_MAX_MEMBERS]; } LocalRoom;
typedef struct {
    int used; uint32_t req_id, token; uint16_t type; uint8_t subtype;
    RequestCallback cb; void *arg; uint16_t app_req_id; int64_t deadline;
    uint64_t room_id; uint16_t member_id;
} Pending;
typedef struct Context {
    int used, started, stopping; uint16_t id; int32_t user_id; uint32_t service_label;
    SOCKET sock; pthread_t reader; int reader_running;
    uint16_t client_id, server_id; uint32_t world_id; uint64_t session;
    uint32_t server_ip; uint16_t udp_port;
    RoomEventCallback room_cb; void *room_arg;
    RoomMessageCallback msg_cb; void *msg_arg;
    SignalingCallback sig_cb; void *sig_arg;
    LocalRoom rooms[MAX_ROOMS];
    Pending pending[MAX_PENDING];
    uint32_t next_req, next_token;
    int start_event_pending, destroy_when_done;
} Context;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Context contexts[MAX_CONTEXTS];
static ContextCallback context_cb; static void *context_arg;
static int initialized, next_ctx = 1;
static size_t stat_requests, stat_events, stat_callbacks;
static GRequestOptParam default_opt;

static int64_t now_us(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000 + t.tv_nsec / 1000; }
static uint64_t rtc_now(void) { return (uint64_t)time(NULL) * 1000000 + UINT64_C(62135596800000000); } /* SceRtcTick: us since year 1 */
static Context *context_of(uint16_t id) {
    for (int i = 0; i < MAX_CONTEXTS; ++i) if (contexts[i].used && contexts[i].id == id) return &contexts[i];
    return NULL;
}
static LocalRoom *room_of(Context *c, uint64_t room_id) {
    for (int i = 0; i < MAX_ROOMS; ++i) if (c->rooms[i].used && c->rooms[i].room.room_id == room_id) return &c->rooms[i];
    return NULL;
}
static int member_index(const WireRoom *r, uint16_t member_id) {
    for (uint16_t i = 0; i < r->member_count; ++i) if (r->members[i].member_id == member_id) return i;
    return -1;
}

/* ---- guest structure builders ---- */
static void build_bin(Arena *a, const WireBinList *list, GBinAttr **out, uint64_t *count) {
    GBinAttr *attrs = arena_alloc(a, sizeof(GBinAttr) * (list->count ? list->count : 1));
    for (uint16_t i = 0; i < list->count && attrs; ++i) {
        void *data = arena_alloc(a, attr_buffer(list->items[i].size));
        if (data) memcpy(data, list->items[i].data, list->items[i].size);
        attrs[i].id = list->items[i].id; attrs[i].data = data; attrs[i].size = list->items[i].size;
    }
    *out = attrs; *count = attrs ? list->count : 0;
}
static void build_int(Arena *a, const WireIntList *list, GIntAttr **out, uint64_t *count) {
    GIntAttr *attrs = arena_alloc(a, sizeof(GIntAttr) * (list->count ? list->count : 1));
    for (uint16_t i = 0; i < list->count && attrs; ++i) { attrs[i].id = list->items[i].id; attrs[i].value = list->items[i].value; }
    *out = attrs; *count = attrs ? list->count : 0;
}
static GMember *build_member(Arena *a, const WireMember *m, int is_owner) {
    GMember *g = arena_alloc(a, sizeof(*g));
    if (!g) return NULL;
    g->join_date = m->join_date; g->account_id = m->account_id; g->platform = m->platform ? m->platform : PLATFORM_PS4;
    memcpy(g->online_id, m->online_id, strnlen(m->online_id, 16));
    g->member_id = m->member_id; g->team_id = m->team_id; g->nat_type = m->nat_type ? m->nat_type : 2;
    g->flag_attr = m->flag_attr | (is_owner ? WIRE_MEMBER_FLAG_OWNER : 0);
    g->attrs = arena_alloc(a, sizeof(GMemberBinAttr) * (m->attrs.count ? m->attrs.count : 1));
    for (uint16_t i = 0; i < m->attrs.count && g->attrs; ++i) {
        void *data = arena_alloc(a, attr_buffer(m->attrs.items[i].size));
        if (data) memcpy(data, m->attrs.items[i].data, m->attrs.items[i].size);
        g->attrs[i].update_date = m->join_date; g->attrs[i].id = m->attrs.items[i].id; g->attrs[i].data = data; g->attrs[i].size = m->attrs.items[i].size;
    }
    g->attr_count = g->attrs ? m->attrs.count : 0;
    return g;
}
static GRoomInternal *build_room_internal(Arena *a, const WireRoom *r) {
    GRoomInternal *g = arena_alloc(a, sizeof(*g));
    if (!g) return NULL;
    uint32_t open = r->max_slot > r->member_count ? r->max_slot - r->member_count : 0;
    g->public_slots = (uint8_t)r->max_slot; g->open_slots = (uint16_t)open; g->cur_members = r->member_count;
    g->max_slot = (uint16_t)r->max_slot; g->server_id = r->server_id; g->world_id = r->world_id;
    g->lobby_id = r->lobby_id; g->room_id = r->room_id; g->password_slot_mask = r->password_slot_mask;
    g->joined_slot_mask = r->joined_slot_mask; g->flag_attr = r->flag_attr;
    g->attrs = arena_alloc(a, sizeof(GRoomBinAttr) * (r->bin_internal.count ? r->bin_internal.count : 1));
    for (uint16_t i = 0; i < r->bin_internal.count && g->attrs; ++i) {
        void *data = arena_alloc(a, attr_buffer(r->bin_internal.items[i].size));
        if (data) memcpy(data, r->bin_internal.items[i].data, r->bin_internal.items[i].size);
        g->attrs[i].update_date = rtc_now(); g->attrs[i].update_member = r->owner_id;
        g->attrs[i].id = r->bin_internal.items[i].id; g->attrs[i].data = data; g->attrs[i].size = r->bin_internal.items[i].size;
    }
    g->attr_count = g->attrs ? r->bin_internal.count : 0;
    return g;
}
/* The full room as the toolkit receives it from CreateJoinRoom, JoinRoom and GetRoomDataInternal. */
static GRoomDataInternal *build_room(Arena *a, const WireRoom *r) {
    GRoomDataInternal *g = arena_alloc(a, sizeof(*g));
    if (!g) return NULL;
    g->room = build_room_internal(a, r);
    GMember *prev = NULL;
    for (uint16_t i = 0; i < r->member_count; ++i) {
        GMember *m = build_member(a, &r->members[i], r->members[i].member_id == r->owner_id);
        if (!m) break;
        if (prev) prev->next = m; else g->members = m;
        prev = m;
        if (m->member_id == r->me_id) g->me = m;
        if (m->member_id == r->owner_id) g->owner = m;
    }
    g->member_count = r->member_count;
    if (!g->owner) g->owner = g->members;
    if (!g->me) g->me = g->members;
    return g;
}
static GRoomExternal *build_room_external(Arena *a, const WireRoom *r) {
    GRoomExternal *g = arena_alloc(a, sizeof(*g));
    if (!g) return NULL;
    uint32_t open = r->max_slot > r->member_count ? r->max_slot - r->member_count : 0;
    g->max_slot = (uint16_t)r->max_slot; g->cur_members = r->member_count; g->flag_attr = r->flag_attr;
    g->server_id = r->server_id; g->world_id = r->world_id; g->lobby_id = r->lobby_id; g->room_id = r->room_id;
    g->password_slot_mask = r->password_slot_mask; g->joined_slot_mask = r->joined_slot_mask;
    g->public_slots = (uint8_t)r->max_slot; g->open_public = (uint8_t)open;
    int owner = member_index(r, r->owner_id);
    if (owner >= 0) {
        g->owner_account = r->members[owner].account_id; g->owner_platform = PLATFORM_PS4;
        memcpy(g->owner_online_id, r->members[owner].online_id, strnlen(r->members[owner].online_id, 16));
    }
    build_int(a, &r->searchable_int, &g->int_attrs, &g->int_attr_count);
    build_bin(a, &r->searchable_bin, &g->sbin_attrs, &g->sbin_attr_count);
    build_bin(a, &r->bin_external, &g->bin_attrs, &g->bin_attr_count);
    return g;
}

/* ---- the request side: reading guest requests into wire messages ---- */
static void read_bin_list(const GBinAttr *attrs, uint64_t count, WireBinList *out) {
    memset(out, 0, sizeof(*out));
    for (uint64_t i = 0; attrs && i < count && i < WIRE_MAX_ATTRS; ++i)
        wire_bin_set(out, attrs[i].id, attrs[i].data, attrs[i].size > WIRE_MAX_ATTR_SIZE ? WIRE_MAX_ATTR_SIZE : (uint32_t)attrs[i].size);
}
static void read_int_list(const GIntAttr *attrs, uint64_t count, WireIntList *out) {
    memset(out, 0, sizeof(*out));
    for (uint64_t i = 0; attrs && i < count && i < WIRE_MAX_ATTRS; ++i) wire_int_set(out, attrs[i].id, attrs[i].value);
}

/* ---- transport ---- */
static int send_all(SOCKET s, const uint8_t *data, size_t size) {
    while (size) {
        int n = send(s, (const char *)data, (int)(size > 65536 ? 65536 : size), 0);
        if (n <= 0) return -1;
        data += n; size -= (size_t)n;
    }
    return 0;
}
/* Reads one frame (blocking, with a timeout in ms; 0 = no frame yet, -1 = connection lost). */
static int read_frame(SOCKET s, int timeout_ms, uint16_t *type, uint32_t *token, uint8_t **payload, size_t *size) {
    fd_set set; FD_ZERO(&set); FD_SET(s, &set);
    struct timeval tv = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
    int ready = select((int)s + 1, &set, NULL, NULL, &tv);
    if (ready == 0) return 0;
    if (ready < 0) return -1;
    uint8_t header[WIRE_HEADER]; size_t got = 0;
    while (got < WIRE_HEADER) {
        int n = recv(s, (char *)header + got, (int)(WIRE_HEADER - got), 0);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    int64_t length = wire_parse_header(header, type, token);
    if (length < 0) return -1;
    uint8_t *data = malloc((size_t)length + 1);
    if (!data) return -1;
    got = 0;
    while (got < (size_t)length) {
        int n = recv(s, (char *)data + got, (int)((size_t)length - got), 0);
        if (n <= 0) { free(data); return -1; }
        got += (size_t)n;
    }
    *payload = data; *size = (size_t)length;
    return 1;
}
static int parse_server_address(const char *text, uint32_t *ip, uint16_t *port) {
    char host[256]; const char *colon = strrchr(text, ':');
    size_t n = colon ? (size_t)(colon - text) : strlen(text);
    if (n >= sizeof(host) || !n) return -1;
    memcpy(host, text, n); host[n] = 0;
    *port = colon ? (uint16_t)atoi(colon + 1) : WIRE_DEFAULT_PORT;
    if (!*port) *port = WIRE_DEFAULT_PORT;
    struct in_addr addr;
    if (inet_pton(AF_INET, host, &addr) == 1) { *ip = ntohl(addr.s_addr); return 0; }
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints)); hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(host, NULL, &hints, &result) || !result) return -1;
    *ip = ntohl(((struct sockaddr_in *)result->ai_addr)->sin_addr.s_addr);
    freeaddrinfo(result);
    return 0;
}
/* Connects to the match server and completes the HELLO/WELCOME exchange (lock not held). */
static int32_t connect_server(Context *c) {
    const char *server = getenv("GOW3_NET_SERVER"), *host = getenv("GOW3_NET_HOST");
    char address[300];
    if (host && *host && *host != '0') {
        const char *port = getenv("GOW3_NET_PORT");
        snprintf(address, sizeof(address), "127.0.0.1:%u", port && atoi(port) ? (unsigned)atoi(port) : WIRE_DEFAULT_PORT);
        server = address;
    }
    if (!server || !*server) { printf("Matching: no server (GOW3_NET_SERVER / GOW3_NET_HOST)\n"); return M2_ERROR_CONNECTION; }
    uint32_t ip; uint16_t port;
    if (parse_server_address(server, &ip, &port)) { printf("Matching: cannot resolve the server %s\n", server); return M2_ERROR_CONNECTION; }
    SOCKET s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == INVALID_SOCKET) return M2_ERROR_CONNECTION;
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(ip); addr.sin_port = htons(port);
#ifdef _WIN32
    DWORD timeout = 5000;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&timeout, sizeof(timeout));
#else
    struct timeval timeout = {5, 0};
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof(one));
    if (connect(s, (struct sockaddr *)&addr, sizeof(addr))) {
        printf("Matching: cannot connect to the server %s\n", server);
        closesocket(s); return M2_ERROR_CONNECTION;
    }
    const OnlineIdentity *me = runtime_online_identity();
    WireBuf b; wire_writer_init(&b);
    wire_begin(&b, WIRE_HELLO, 0);
    wire_put_u32(&b, WIRE_VERSION); wire_put_u64(&b, me->account_id); wire_put_string(&b, me->online_id);
    wire_put_u32(&b, PLATFORM_PS4); wire_put_u16(&b, me->client_id); wire_put_u16(&b, runtime_net_p2p_port());
    wire_finish(&b);
    if (b.error || send_all(s, b.data, b.size)) { wire_free(&b); closesocket(s); return M2_ERROR_CONNECTION; }
    wire_free(&b);
    uint16_t type; uint32_t token; uint8_t *payload; size_t size;
    int r = read_frame(s, 5000, &type, &token, &payload, &size);
    if (r <= 0) { printf("Matching: no answer from the server %s\n", server); closesocket(s); return M2_ERROR_CONNECTION; }
    WireBuf in; wire_reader_init(&in, payload, size);
    int32_t result = M2_ERROR_CONNECTION;
    if (type == WIRE_WELCOME) {
        uint32_t version = wire_get_u32(&in);
        c->client_id = wire_get_u16(&in); c->session = wire_get_u64(&in); c->udp_port = wire_get_u16(&in);
        c->server_id = wire_get_u16(&in); c->world_id = wire_get_u32(&in);
        char motd[256]; wire_get_string(&in, motd, sizeof(motd));
        if (!in.error && version == WIRE_VERSION) {
            c->sock = s; c->server_ip = ip;
            runtime_online_set_client_id(c->client_id);
            runtime_net_relay_configure(ip, c->udp_port, c->client_id, c->session);
            printf("Matching: connected to %s as %s (%s)\n", server, runtime_online_identity()->online_id, motd[0] ? motd : "no message of the day");
            result = 0;
        } else printf("Matching: the server %s speaks protocol %u, this game %u\n", server, version, WIRE_VERSION);
    } else if (type == WIRE_ERROR) {
        int32_t code = (int32_t)wire_get_u32(&in); char text[256]; wire_get_string(&in, text, sizeof(text));
        printf("Matching: the server refused: %s (%#x)\n", text, (unsigned)code);
        result = code ? code : M2_ERROR_CONNECTION;
    }
    free(payload);
    if (result) closesocket(s);
    return result;
}

/* ---- callbacks (lock never held while calling into the toolkit) ---- */
static void deliver_context(uint16_t ctx, uint16_t event, int32_t error) {
    pthread_mutex_lock(&lock); ContextCallback cb = context_cb; void *arg = context_arg; pthread_mutex_unlock(&lock);
    TRACE("context event %#x error %#x -> %s", event, (unsigned)error, cb ? "callback" : "nobody");
    if (cb) { ++stat_callbacks; cb(ctx, event, 0, error, arg); }
}
static void deliver_request(Context *c, const Pending *p, int32_t error, const void *data) {
    TRACE("request %u (type %u) -> callback error %#x", p->req_id, p->type, (unsigned)error);
    if (p->cb) { ++stat_callbacks; p->cb(c->id, p->req_id, EV_REQUEST, error, data, p->arg); }
}
static void trace_attrs(const char *label, const GBinAttr *attrs, uint64_t count) {
    if (!runtime_online_trace()) return;
    for (uint64_t i = 0; attrs && i < count && i < WIRE_MAX_ATTRS; ++i) {
        char text[64]; snprintf(text, sizeof(text), "%s %#x", label, attrs[i].id);
        runtime_online_dump(text, attrs[i].data, (size_t)attrs[i].size);
    }
}
static void trace_int_attrs(const char *label, const GIntAttr *attrs, uint64_t count) {
    if (!runtime_online_trace()) return;
    for (uint64_t i = 0; attrs && i < count && i < WIRE_MAX_ATTRS; ++i) TRACE("%s %#x = %u (%#x)", label, attrs[i].id, attrs[i].value, attrs[i].value);
}
static void trace_bin_list(const char *label, const WireBinList *l) {
    if (!runtime_online_trace()) return;
    for (uint16_t i = 0; i < l->count; ++i) {
        char text[64]; snprintf(text, sizeof(text), "%s %#x", label, l->items[i].id);
        runtime_online_hex(text, l->items[i].data, l->items[i].size, 32);
    }
}
static void deliver_room_event(Context *c, uint64_t room_id, uint16_t event, const void *data) {
    pthread_mutex_lock(&lock); RoomEventCallback cb = c->room_cb; void *arg = c->room_arg; pthread_mutex_unlock(&lock);
    TRACE("room %llu event %#x -> %s", (unsigned long long)room_id, event, cb ? "callback" : "nobody");
    if (cb) { ++stat_callbacks; cb(c->id, room_id, event, data, arg); }
}
static void deliver_signaling(Context *c, uint64_t room_id, uint16_t peer, uint16_t event, int32_t error) {
    pthread_mutex_lock(&lock); SignalingCallback cb = c->sig_cb; void *arg = c->sig_arg; pthread_mutex_unlock(&lock);
    TRACE("signaling room %llu member %u event %#x", (unsigned long long)room_id, peer, event);
    if (cb) { ++stat_callbacks; cb(c->id, room_id, peer, event, error, arg); }
}
static void deliver_message(Context *c, uint64_t room_id, uint16_t src, uint16_t event, const GRoomMessageInfo *info) {
    pthread_mutex_lock(&lock); RoomMessageCallback cb = c->msg_cb; void *arg = c->msg_arg; pthread_mutex_unlock(&lock);
    TRACE("room %llu message from %u (%s, %llu bytes)", (unsigned long long)room_id, src, event == EV_ROOM_MSG_CHAT ? "chat" : "data", (unsigned long long)info->msg_len);
    if (cb) { ++stat_callbacks; cb(c->id, room_id, src, event, info, arg); }
}

/* ---- responses ---- */
static Pending take_pending(Context *c, uint32_t token, int *found) {
    Pending p; memset(&p, 0, sizeof(p)); *found = 0;
    pthread_mutex_lock(&lock);
    for (int i = 0; i < MAX_PENDING; ++i)
        if (c->pending[i].used && c->pending[i].token == token) { p = c->pending[i]; c->pending[i].used = 0; *found = 1; break; }
    pthread_mutex_unlock(&lock);
    return p;
}
static void store_room(Context *c, const WireRoom *room) {
    pthread_mutex_lock(&lock);
    LocalRoom *r = room_of(c, room->room_id);
    if (!r) for (int i = 0; i < MAX_ROOMS && !r; ++i) if (!c->rooms[i].used) { r = &c->rooms[i]; memset(r, 0, sizeof(*r)); r->used = 1; }
    if (r) {
        SignalingState sig[WIRE_MAX_MEMBERS]; memcpy(sig, r->sig, sizeof(sig));
        WireRoom old = r->room;
        r->room = *room;
        memset(r->sig, 0, sizeof(r->sig));
        for (uint16_t i = 0; i < room->member_count; ++i) { /* keep the signaling state by member id */
            int j = member_index(&old, room->members[i].member_id);
            if (j >= 0 && old.room_id == room->room_id) r->sig[i] = sig[j];
        }
    }
    pthread_mutex_unlock(&lock);
    runtime_presence_room(room->room_id, room->member_count, room->max_slot);
}
static void forget_room(Context *c, uint64_t room_id) {
    pthread_mutex_lock(&lock);
    LocalRoom *r = room_of(c, room_id);
    if (r) r->used = 0;
    pthread_mutex_unlock(&lock);
    runtime_presence_room(room_id, 0, 0);
}
static void handle_response(Context *c, uint16_t type, uint32_t token, WireBuf *in) {
    int found; Pending p = take_pending(c, token, &found);
    if (!found) { TRACE("response %u for an unknown request (token %u)", type, token); return; }
    if (type == WIRE_ERROR) {
        int32_t code = (int32_t)wire_get_u32(in); char text[256]; wire_get_string(in, text, sizeof(text));
        printf("Matching: request %u failed: %s (%#x)\n", p.req_id, text, (unsigned)code);
        if (p.type == WIRE_CREATE_ROOM || p.type == WIRE_JOIN_ROOM) { /* nothing was joined */ }
        deliver_request(c, &p, code ? code : M2_ERROR_CONNECTION, NULL);
        return;
    }
    Arena *a = arena_new(ARENA_ROOM * 2);
    if (!a) { deliver_request(c, &p, M2_ERROR_NO_MEMORY, NULL); return; }
    const void *data = NULL; int32_t error = 0;
    switch (p.type) {
    case WIRE_GET_WORLDS: {
        GWorldList *list = arena_alloc(a, sizeof(*list));
        uint32_t n = wire_get_u32(in);
        GWorld *prev = NULL;
        for (uint32_t i = 0; i < n && i < 16 && list; ++i) {
            GWorld *w = arena_alloc(a, sizeof(*w));
            if (!w) break;
            w->world_id = wire_get_u32(in); w->cur_rooms = wire_get_u32(in); w->cur_room_members = wire_get_u32(in);
            w->max_lobby_members = 64;
            if (prev) prev->next = w; else list->worlds = w;
            prev = w; ++list->count;
        }
        data = list; break;
    }
    case WIRE_CREATE_ROOM: case WIRE_JOIN_ROOM: case WIRE_GET_ROOM_INTERNAL: {
        WireRoom *room = malloc(sizeof(*room));
        if (!room) { error = M2_ERROR_NO_MEMORY; break; }
        wire_get_room(in, room);
        if (in->error) { free(room); error = M2_ERROR_CONNECTION; break; }
        if (p.type != WIRE_GET_ROOM_INTERNAL) {
            store_room(c, room);
            printf("Matching: %s room %llu (%u of %u players)\n", p.type == WIRE_CREATE_ROOM ? "created" : "joined",
                   (unsigned long long)room->room_id, room->member_count, room->max_slot);
        }
        data = build_room(a, room);
        free(room);
        break;
    }
    case WIRE_SEARCH: case WIRE_GET_ROOM_EXTERNAL: {
        uint32_t start = wire_get_u32(in), total = wire_get_u32(in), n = wire_get_u32(in);
        GRoomExternal *prev = NULL, *head = NULL;
        WireRoom *room = malloc(sizeof(*room));
        uint32_t built = 0;
        for (uint32_t i = 0; i < n && room; ++i) {
            wire_get_room(in, room);
            if (in->error) break;
            GRoomExternal *g = build_room_external(a, room);
            if (!g) break;
            if (prev) prev->next = g; else head = g;
            prev = g; ++built;
        }
        free(room);
        if (p.type == WIRE_SEARCH) {
            GSearchResponse *r = arena_alloc(a, sizeof(*r));
            if (r) { r->start = start; r->total = total; r->size = built; r->rooms = head; }
            data = r;
            TRACE("search: %u rooms", built);
        } else {
            GExternalListResponse *r = arena_alloc(a, sizeof(*r));
            if (r) r->rooms = head;
            data = r;
        }
        break;
    }
    case WIRE_GET_MEMBER: {
        WireMember *m = malloc(sizeof(*m));
        if (!m) { error = M2_ERROR_NO_MEMORY; break; }
        wire_get_member(in, m);
        GMemberResponse *r = arena_alloc(a, sizeof(*r));
        pthread_mutex_lock(&lock);
        LocalRoom *lr = room_of(c, p.room_id);
        int owner = lr && lr->room.owner_id == m->member_id;
        pthread_mutex_unlock(&lock);
        if (r) r->member = build_member(a, m, owner);
        free(m);
        data = r; break;
    }
    case WIRE_PING: {
        GPingResponse *r = arena_alloc(a, sizeof(*r));
        uint32_t rtt = wire_get_u32(in);
        if (r) { r->server_id = c->server_id; r->world_id = c->world_id; r->room_id = p.room_id; r->rtt = rtt ? rtt : 1000; }
        data = r; break;
    }
    case WIRE_LEAVE_ROOM:
        forget_room(c, p.room_id);
        printf("Matching: left room %llu\n", (unsigned long long)p.room_id);
        break;
    default: break; /* OK answers carry nothing */
    }
    deliver_request(c, &p, error, data);
    free(a);
}

/* ---- events ---- */
static void opt_data(GOptData *out, WireBuf *in) {
    uint32_t n = wire_get_blob(in, out->data, sizeof(out->data));
    out->length = n;
}
static void handle_event(Context *c, uint16_t type, WireBuf *in) {
    ++stat_events;
    uint64_t room_id = wire_get_u64(in);
    Arena *a = arena_new(ARENA_ROOM);
    if (!a) return;
    switch (type) {
    case WIRE_EV_JOINED: case WIRE_EV_LEFT: {
        WireMember m; wire_get_member(in, &m);
        uint8_t cause = wire_get_u8(in);
        GMemberUpdateInfo *info = arena_alloc(a, sizeof(*info));
        pthread_mutex_lock(&lock);
        LocalRoom *r = room_of(c, room_id);
        int owner = r && r->room.owner_id == m.member_id;
        if (r) {
            int j = member_index(&r->room, m.member_id);
            if (type == WIRE_EV_JOINED) {
                if (j < 0 && r->room.member_count < WIRE_MAX_MEMBERS) { j = r->room.member_count++; memset(&r->sig[j], 0, sizeof(r->sig[j])); }
                if (j >= 0) r->room.members[j] = m;
            } else if (j >= 0) {
                for (int k = j; k + 1 < r->room.member_count; ++k) { r->room.members[k] = r->room.members[k + 1]; r->sig[k] = r->sig[k + 1]; }
                --r->room.member_count;
            }
        }
        const unsigned members = r ? r->room.member_count : 0, slots = r ? r->room.max_slot : 0;
        pthread_mutex_unlock(&lock);
        if (r) runtime_presence_room(room_id, members, slots);
        if (info) { info->member = build_member(a, &m, owner); info->cause = cause; opt_data(&info->opt, in); }
        printf("Matching: %s %s room %llu\n", m.online_id, type == WIRE_EV_JOINED ? "joined" : "left", (unsigned long long)room_id);
        deliver_room_event(c, room_id, type == WIRE_EV_JOINED ? EV_ROOM_MEMBER_JOINED : EV_ROOM_MEMBER_LEFT, info);
        break;
    }
    case WIRE_EV_KICKED: case WIRE_EV_DESTROYED: {
        GRoomUpdateInfo *info = arena_alloc(a, sizeof(*info));
        uint8_t cause = wire_get_u8(in);
        if (info) { info->cause = cause; info->error = 0; opt_data(&info->opt, in); }
        forget_room(c, room_id);
        printf("Matching: %s room %llu\n", type == WIRE_EV_KICKED ? "kicked out of" : "destroyed:", (unsigned long long)room_id);
        deliver_room_event(c, room_id, type == WIRE_EV_KICKED ? EV_ROOM_KICKED_OUT : EV_ROOM_DESTROYED, info);
        break;
    }
    case WIRE_EV_OWNER: {
        GOwnerUpdateInfo *info = arena_alloc(a, sizeof(*info));
        uint16_t prev = wire_get_u16(in), next = wire_get_u16(in); uint8_t cause = wire_get_u8(in);
        uint8_t has_password = wire_get_u8(in); uint64_t password = wire_get_u64(in);
        pthread_mutex_lock(&lock);
        LocalRoom *r = room_of(c, room_id);
        if (r) { r->room.owner_id = next; for (uint16_t i = 0; i < r->room.member_count; ++i) r->room.members[i].flag_attr = r->room.members[i].member_id == next ? WIRE_MEMBER_FLAG_OWNER : 0; }
        pthread_mutex_unlock(&lock);
        if (info) {
            info->prev_owner = prev; info->new_owner = next; info->cause = cause;
            if (has_password) { uint64_t *p = arena_alloc(a, 8); if (p) { *p = password; info->password = p; } }
            opt_data(&info->opt, in);
        }
        printf("Matching: room %llu owner is now member %u\n", (unsigned long long)room_id, next);
        deliver_room_event(c, room_id, EV_ROOM_OWNER_UPDATED, info);
        break;
    }
    case WIRE_EV_ROOM_UPDATED: {
        WireRoom *room = malloc(sizeof(*room));
        if (!room) break;
        uint32_t prev_flags = wire_get_u32(in);
        wire_get_room(in, room);
        if (in->error) { free(room); break; }
        pthread_mutex_lock(&lock);
        LocalRoom *r = room_of(c, room_id);
        if (r) { room->me_id = r->room.me_id; }
        pthread_mutex_unlock(&lock);
        store_room(c, room);
        GRoomDataUpdateInfo *info = arena_alloc(a, sizeof(*info));
        if (info) {
            info->room = build_room_internal(a, room);
            uint32_t *flags = arena_alloc(a, 8);
            if (flags) { flags[0] = room->flag_attr; flags[1] = prev_flags; info->new_flags = flags; info->prev_flags = flags + 1; }
            info->slot_info = info->room;
            if (info->room && info->room->attr_count) {
                GRoomBinAttr **list = arena_alloc(a, sizeof(void *) * info->room->attr_count);
                for (uint64_t i = 0; list && i < info->room->attr_count; ++i) list[i] = &info->room->attrs[i];
                info->attrs = list; info->attr_count = list ? info->room->attr_count : 0;
            }
        }
        TRACE("room %llu data updated: flags %#x (was %#x), %u attrs", (unsigned long long)room_id, room->flag_attr, prev_flags, room->bin_internal.count);
        trace_bin_list("  room attr", &room->bin_internal);
        free(room);
        deliver_room_event(c, room_id, EV_ROOM_DATA_INTERNAL_UPDATED, info);
        break;
    }
    case WIRE_EV_MEMBER_UPDATED: {
        WireMember m; wire_get_member(in, &m);
        pthread_mutex_lock(&lock);
        LocalRoom *r = room_of(c, room_id);
        int owner = r && r->room.owner_id == m.member_id;
        if (r) { int j = member_index(&r->room, m.member_id); if (j >= 0) r->room.members[j] = m; }
        pthread_mutex_unlock(&lock);
        GMemberDataUpdateInfo *info = arena_alloc(a, sizeof(*info));
        if (info) {
            info->member = build_member(a, &m, owner);
            if (info->member) {
                info->new_flags = &info->member->flag_attr; info->new_team = &info->member->team_id;
                GMemberBinAttr **list = arena_alloc(a, sizeof(void *) * (info->member->attr_count ? info->member->attr_count : 1));
                for (uint64_t i = 0; list && i < info->member->attr_count; ++i) list[i] = &info->member->attrs[i];
                info->attrs = list; info->attr_count = list ? info->member->attr_count : 0;
            }
        }
        TRACE("room %llu member %u data updated: team %u, flags %#x, %u attrs", (unsigned long long)room_id, m.member_id, m.team_id, m.flag_attr, m.attrs.count);
        trace_bin_list("  member attr", &m.attrs);
        deliver_room_event(c, room_id, EV_ROOM_MEMBER_DATA_INTERNAL_UPDATED, info);
        break;
    }
    case WIRE_EV_MESSAGE: {
        uint16_t src = wire_get_u16(in); uint8_t cast = wire_get_u8(in), chat = wire_get_u8(in);
        uint32_t size = wire_get_u32(in);
        if (in->error || size > WIRE_MAX_MESSAGE || in->pos + size > in->size) break;
        GRoomMessageInfo *info = arena_alloc(a, sizeof(*info));
        void *msg = arena_alloc(a, size ? size : 1);
        if (info && msg) {
            memcpy(msg, in->data + in->pos, size);
            info->cast_type = cast; info->msg = msg; info->msg_len = size;
            runtime_online_dump("  message in", msg, size);
            deliver_message(c, room_id, src, chat ? EV_ROOM_MSG_CHAT : EV_ROOM_MSG_MESSAGE, info);
        }
        break;
    }
    case WIRE_EV_SIGNALING: {
        uint16_t peer = wire_get_u16(in); uint8_t established = wire_get_u8(in);
        uint16_t peer_client = wire_get_u16(in), peer_port = wire_get_u16(in); uint32_t rtt = wire_get_u32(in);
        pthread_mutex_lock(&lock);
        LocalRoom *r = room_of(c, room_id);
        if (r) {
            int j = member_index(&r->room, peer);
            if (j >= 0) {
                r->sig[j].status = established ? SIG_ESTABLISHED : SIG_DEAD; r->sig[j].rtt_us = rtt;
                if (peer_client) r->room.members[j].client_id = peer_client;
                if (peer_port) r->room.members[j].p2p_port = peer_port;
            }
        }
        pthread_mutex_unlock(&lock);
        deliver_signaling(c, room_id, peer, established ? EV_SIGNALING_ESTABLISHED : EV_SIGNALING_DEAD, 0);
        break;
    }
    default: TRACE("unknown event %u", type); break;
    }
    free(a);
}
static void fail_pending(Context *c, int64_t now, int all) {
    for (;;) {
        Pending p; int found = 0;
        pthread_mutex_lock(&lock);
        for (int i = 0; i < MAX_PENDING; ++i)
            if (c->pending[i].used && (all || c->pending[i].deadline <= now)) { p = c->pending[i]; c->pending[i].used = 0; found = 1; break; }
        pthread_mutex_unlock(&lock);
        if (!found) return;
        printf("Matching: request %u %s\n", p.req_id, all ? "abandoned: connection lost" : "timed out");
        deliver_request(c, &p, all ? M2_ERROR_CONNECTION : M2_ERROR_TIMEOUT, NULL);
    }
}
static void *reader_thread(void *arg) {
    Context *c = arg;
    runtime_thread_attach_host("NpMatching2");
    if (c->start_event_pending) { c->start_event_pending = 0; deliver_context(c->id, EV_CONTEXT_START, 0); }
    while (!c->stopping) {
        uint16_t type; uint32_t token; uint8_t *payload; size_t size;
        int r = read_frame(c->sock, 100, &type, &token, &payload, &size);
        if (r < 0) {
            if (!c->stopping) {
                printf("Matching: connection to the server lost\n");
                fail_pending(c, 0, 1);
                for (int i = 0; i < MAX_ROOMS; ++i) {
                    pthread_mutex_lock(&lock); int used = c->rooms[i].used; uint64_t id = c->rooms[i].room.room_id; c->rooms[i].used = 0; pthread_mutex_unlock(&lock);
                    if (!used) continue;
                    Arena *a = arena_new(256);
                    GRoomUpdateInfo *info = a ? arena_alloc(a, sizeof(*info)) : NULL;
                    if (info) info->cause = WIRE_CAUSE_CONNECTION_FAILURE;
                    deliver_room_event(c, id, EV_ROOM_DESTROYED, info);
                    free(a);
                }
                deliver_context(c->id, EV_CONTEXT_STOP, M2_ERROR_CONNECTION);
            }
            break;
        }
        if (r == 0) { fail_pending(c, now_us(), 0); continue; }
        WireBuf in; wire_reader_init(&in, payload, size);
        if (token) handle_response(c, type, token, &in);
        else handle_event(c, type, &in);
        free(payload);
    }
    pthread_mutex_lock(&lock);
    c->reader_running = 0;
    if (c->destroy_when_done) { c->destroy_when_done = 0; c->used = 0; }
    pthread_mutex_unlock(&lock);
    return NULL;
}

/* ---- sending requests ---- */
static int32_t submit(uint16_t ctx_id, uint16_t type, uint8_t subtype, const GRequestOptParam *opt, uint32_t *assigned,
                      uint64_t room_id, uint16_t member_id, WireBuf *b) {
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id);
    if (!c || !c->started) { pthread_mutex_unlock(&lock); wire_free(b); return c ? M2_ERROR_CONTEXT_NOT_STARTED : M2_ERROR_CONTEXT_NOT_FOUND; }
    Pending *p = NULL;
    for (int i = 0; i < MAX_PENDING; ++i) if (!c->pending[i].used) { p = &c->pending[i]; break; }
    if (!p) { pthread_mutex_unlock(&lock); wire_free(b); return M2_ERROR_NO_MEMORY; }
    memset(p, 0, sizeof(*p));
    p->used = 1; p->req_id = c->next_req++; p->token = c->next_token++; p->type = type; p->subtype = subtype;
    if (!c->next_token) c->next_token = 1;
    const GRequestOptParam *o = opt ? opt : &default_opt;
    p->cb = o->cb; p->arg = o->arg; p->app_req_id = o->app_req_id;
    p->deadline = now_us() + (o->timeout ? (int64_t)o->timeout : 20000000);
    p->room_id = room_id; p->member_id = member_id;
    if (assigned) *assigned = p->req_id;
    uint32_t req_id = p->req_id, token = p->token;
    /* patch the token into the header */
    for (int i = 0; i < 4; ++i) b->data[8 + i] = (uint8_t)(token >> (8 * i));
    ++stat_requests;
    SOCKET s = c->sock;
    pthread_mutex_unlock(&lock);
    TRACE("request %u: wire %u to room %llu", req_id, type, (unsigned long long)room_id);
    wire_finish(b);
    int failed = b->error || send_all(s, b->data, b->size);
    wire_free(b);
    if (failed) { int found; take_pending(c, token, &found); return M2_ERROR_CONNECTION; }
    return 0;
}
static void put_bin_attrs(WireBuf *b, const GBinAttr *attrs, uint64_t count) { WireBinList l; read_bin_list(attrs, count, &l); wire_put_bin_list(b, &l); }
static void put_int_attrs(WireBuf *b, const GIntAttr *attrs, uint64_t count) { WireIntList l; read_int_list(attrs, count, &l); wire_put_int_list(b, &l); }

/* ---- the API ---- */
static ABI int32_t m2_initialize(const void *param) {
    (void)param;
    pthread_mutex_lock(&lock); initialized = 1; pthread_mutex_unlock(&lock);
    TRACE("sceNpMatching2Initialize");
    return 0;
}
static ABI int32_t m2_terminate(void) {
    pthread_mutex_lock(&lock); initialized = 0; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t m2_create_context(const uint32_t *param, uint16_t *ctx_id) {
    if (!param || !ctx_id) return M2_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    if (!initialized) { pthread_mutex_unlock(&lock); return M2_ERROR_NOT_INITIALIZED; }
    Context *c = NULL;
    for (int i = 0; i < MAX_CONTEXTS; ++i) if (!contexts[i].used) { c = &contexts[i]; break; }
    if (!c) { pthread_mutex_unlock(&lock); return M2_ERROR_CONTEXT_MAX; }
    memset(c, 0, sizeof(*c));
    c->used = 1; c->id = (uint16_t)next_ctx++; c->user_id = (int32_t)param[0]; c->service_label = param[1];
    c->sock = INVALID_SOCKET; c->next_req = 1; c->next_token = 1;
    *ctx_id = c->id;
    pthread_mutex_unlock(&lock);
    TRACE("sceNpMatching2CreateContextA(user %d, label %u) -> %u", c->user_id, c->service_label, c->id);
    return 0;
}
/* Stops a context without waiting for its reader thread: the toolkit calls this with its own
 * lock held, and the reader may be inside a callback that wants that lock. The reader sees
 * `stopping`, finishes, and releases the context itself when it was destroyed meanwhile. */
static void stop_context(Context *c, int destroy) {
    pthread_mutex_lock(&lock);
    c->started = 0; c->stopping = 1;
    SOCKET s = c->sock; c->sock = INVALID_SOCKET;
    for (int i = 0; i < MAX_ROOMS; ++i) c->rooms[i].used = 0;
    for (int i = 0; i < MAX_PENDING; ++i) c->pending[i].used = 0;
    if (destroy) { if (c->reader_running) c->destroy_when_done = 1; else c->used = 0; }
    pthread_mutex_unlock(&lock);
    runtime_presence_room(0, 0, 0);
    if (s != INVALID_SOCKET) { shutdown(s, 2); closesocket(s); }
}
static ABI int32_t m2_destroy_context(uint16_t ctx_id) {
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); pthread_mutex_unlock(&lock);
    if (!c) return M2_ERROR_CONTEXT_NOT_FOUND;
    stop_context(c, 1);
    TRACE("sceNpMatching2DestroyContext(%u)", ctx_id);
    return 0;
}
static ABI int32_t m2_context_start(uint16_t ctx_id, uint64_t timeout) {
    (void)timeout;
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id);
    if (!c) { pthread_mutex_unlock(&lock); return M2_ERROR_CONTEXT_NOT_FOUND; }
    if (c->started) { pthread_mutex_unlock(&lock); return M2_ERROR_CONTEXT_ALREADY_STARTED; }
    int busy = c->reader_running;
    pthread_mutex_unlock(&lock);
    if (busy) return M2_ERROR_CONTEXT_ALREADY_STARTED; /* the previous connection is still closing */
    int32_t r = connect_server(c);
    if (r) { TRACE("sceNpMatching2ContextStart(%u) failed %#x", ctx_id, (unsigned)r); return r; }
    pthread_mutex_lock(&lock);
    c->started = 1; c->stopping = 0; c->start_event_pending = 1; c->reader_running = 1;
    if (pthread_create(&c->reader, NULL, reader_thread, c)) { c->started = 0; c->reader_running = 0; closesocket(c->sock); c->sock = INVALID_SOCKET; pthread_mutex_unlock(&lock); return M2_ERROR_NO_MEMORY; }
    pthread_detach(c->reader);
    pthread_mutex_unlock(&lock);
    TRACE("sceNpMatching2ContextStart(%u)", ctx_id);
    return 0;
}
/* The toolkit waits for the Stop event after sceNpMatching2ContextStop returns: a short-lived
 * thread delivers it. */
static void *stop_thread(void *arg) {
    uint16_t id = *(uint16_t *)arg; free(arg);
    runtime_thread_attach_host("NpMatching2Stop");
    deliver_context(id, EV_CONTEXT_STOP, 0);
    return NULL;
}
static ABI int32_t m2_context_stop(uint16_t ctx_id) {
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); int started = c && c->started; pthread_mutex_unlock(&lock);
    if (!c) return M2_ERROR_CONTEXT_NOT_FOUND;
    if (!started) return M2_ERROR_CONTEXT_NOT_STARTED;
    stop_context(c, 0);
    TRACE("sceNpMatching2ContextStop(%u)", ctx_id);
    pthread_t t; uint16_t *id = malloc(sizeof(*id));
    if (id) *id = ctx_id;
    if (id && !pthread_create(&t, NULL, stop_thread, id)) pthread_detach(t);
    else free(id);
    return 0;
}
static ABI int32_t m2_register_context_callback(ContextCallback cb, void *arg) {
    pthread_mutex_lock(&lock); context_cb = cb; context_arg = arg; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t m2_register_room_event_callback(uint16_t ctx_id, RoomEventCallback cb, void *arg) {
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); if (c) { c->room_cb = cb; c->room_arg = arg; } pthread_mutex_unlock(&lock);
    return c ? 0 : M2_ERROR_CONTEXT_NOT_FOUND;
}
static ABI int32_t m2_register_room_message_callback(uint16_t ctx_id, RoomMessageCallback cb, void *arg) {
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); if (c) { c->msg_cb = cb; c->msg_arg = arg; } pthread_mutex_unlock(&lock);
    return c ? 0 : M2_ERROR_CONTEXT_NOT_FOUND;
}
static ABI int32_t m2_register_signaling_callback(uint16_t ctx_id, SignalingCallback cb, void *arg) {
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); if (c) { c->sig_cb = cb; c->sig_arg = arg; } pthread_mutex_unlock(&lock);
    return c ? 0 : M2_ERROR_CONTEXT_NOT_FOUND;
}
static ABI int32_t m2_set_default_opt(const GRequestOptParam *opt) {
    pthread_mutex_lock(&lock); if (opt) default_opt = *opt; else memset(&default_opt, 0, sizeof(default_opt)); pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t m2_get_server_id(uint16_t ctx_id, uint16_t *server_id) {
    if (!server_id) return M2_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock); Context *c = context_of(ctx_id); int started = c && c->started; uint16_t id = c ? c->server_id : 0; pthread_mutex_unlock(&lock);
    if (!c) return M2_ERROR_CONTEXT_NOT_FOUND;
    if (!started) return M2_ERROR_CONTEXT_NOT_STARTED;
    *server_id = id ? id : WIRE_SERVER_ID;
    return 0;
}
static ABI int32_t m2_get_world_list(uint16_t ctx_id, const uint16_t *req, const GRequestOptParam *opt, uint32_t *assigned) {
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_GET_WORLDS, 0);
    wire_put_u16(&b, req ? *req : WIRE_SERVER_ID);
    return submit(ctx_id, WIRE_GET_WORLDS, 0, opt, assigned, 0, 0, &b);
}
static ABI int32_t m2_create_join_room(uint16_t ctx_id, const GCreateJoinRoomRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    if (runtime_online_trace()) {
        const uint8_t *raw = (const uint8_t *)req; char hex[0xC0 * 3 + 1]; int n = 0;
        for (int i = 0; i < 0xC0; ++i) n += snprintf(hex + n, sizeof(hex) - n, "%02x%s", raw[i], (i & 15) == 15 ? "|" : " ");
        TRACE("sceNpMatching2CreateJoinRoomA request bytes: %s", hex);
    }
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_CREATE_ROOM, 0);
    uint32_t max_slot = req->head; /* the toolkit's maxSlot (see docs); 8 players when it reads oddly */
    if (max_slot < 2 || max_slot > WIRE_MAX_MEMBERS) max_slot = 8;
    wire_put_u32(&b, req->world_id); wire_put_u32(&b, max_slot); wire_put_u32(&b, req->flag_attr); wire_put_u64(&b, req->lobby_id);
    wire_put_u8(&b, req->password != NULL); wire_put_u64(&b, req->password ? *req->password : 0);
    wire_put_u64(&b, req->password_slot_mask ? *req->password_slot_mask : 0);
    wire_put_u8(&b, req->sig_opt ? req->sig_opt->type : 1); wire_put_u8(&b, req->sig_opt ? req->sig_opt->flag : 0);
    wire_put_u16(&b, req->sig_opt ? req->sig_opt->hub : 0);
    put_bin_attrs(&b, req->bin_internal, req->bin_internal_count); put_bin_attrs(&b, req->bin_external, req->bin_external_count);
    put_bin_attrs(&b, req->sbin, req->sbin_count); put_int_attrs(&b, req->sint, req->sint_count);
    put_bin_attrs(&b, req->member_bin, req->member_bin_count);
    TRACE("sceNpMatching2CreateJoinRoomA(world %u, slots %u, flags %#x, %s, sig %u)", req->world_id, max_slot, req->flag_attr,
          req->password ? "password" : "open", req->sig_opt ? req->sig_opt->type : 0);
    return submit(ctx_id, WIRE_CREATE_ROOM, 0, opt, assigned, 0, 0, &b);
}
static ABI int32_t m2_join_room(uint16_t ctx_id, const GJoinRoomRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_JOIN_ROOM, 0);
    wire_put_u64(&b, req->room_id); wire_put_u8(&b, req->password != NULL); wire_put_u64(&b, req->password ? *req->password : 0);
    put_bin_attrs(&b, req->member_bin, req->member_bin_count);
    TRACE("sceNpMatching2JoinRoomA(room %llu)", (unsigned long long)req->room_id);
    return submit(ctx_id, WIRE_JOIN_ROOM, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_search_room(uint16_t ctx_id, const GSearchRoomRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireSearch s; memset(&s, 0, sizeof(s));
    s.world_id = req->world_id; s.start_index = req->start_index; s.max = req->max ? req->max : 20;
    s.flag_filter = req->flag_filter; s.flag_attr = req->flag_attr;
    for (uint64_t i = 0; req->int_filters && i < req->int_filter_count && i < WIRE_MAX_ATTRS; ++i) {
        s.int_filters[i].op = req->int_filters[i].op; s.int_filters[i].attr.id = req->int_filters[i].attr.id; s.int_filters[i].attr.value = req->int_filters[i].attr.value;
        ++s.int_filter_count;
    }
    for (uint64_t i = 0; req->bin_filters && i < req->bin_filter_count && i < WIRE_MAX_ATTRS; ++i) {
        const GBinAttr *a = &req->bin_filters[i].attr;
        s.bin_filters[i].op = req->bin_filters[i].op; s.bin_filters[i].attr.id = a->id;
        s.bin_filters[i].attr.size = a->size > WIRE_MAX_ATTR_SIZE ? WIRE_MAX_ATTR_SIZE : (uint32_t)a->size;
        if (a->data) memcpy(s.bin_filters[i].attr.data, a->data, s.bin_filters[i].attr.size);
        ++s.bin_filter_count;
    }
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SEARCH, 0);
    wire_put_search(&b, &s);
    TRACE("sceNpMatching2SearchRoom(world %u, %u int filters, %u bin filters, flags %#x/%#x)", s.world_id, s.int_filter_count, s.bin_filter_count, s.flag_filter, s.flag_attr);
    return submit(ctx_id, WIRE_SEARCH, 0, opt, assigned, 0, 0, &b);
}
static ABI int32_t m2_leave_room(uint16_t ctx_id, const GLeaveRoomRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_LEAVE_ROOM, 0);
    wire_put_u64(&b, req->room_id);
    TRACE("sceNpMatching2LeaveRoom(room %llu)", (unsigned long long)req->room_id);
    return submit(ctx_id, WIRE_LEAVE_ROOM, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_set_room_internal(uint16_t ctx_id, const GSetRoomInternalRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SET_ROOM_INTERNAL, 0);
    wire_put_u64(&b, req->room_id); wire_put_u32(&b, req->flag_filter); wire_put_u32(&b, req->flag_attr);
    put_bin_attrs(&b, req->attrs, req->attr_count);
    wire_put_u8(&b, req->password_slot_mask != NULL); wire_put_u64(&b, req->password_slot_mask ? *req->password_slot_mask : 0);
    TRACE("sceNpMatching2SetRoomDataInternal(room %llu, flags %#x/%#x, %llu attrs)", (unsigned long long)req->room_id, req->flag_filter, req->flag_attr, (unsigned long long)req->attr_count);
    trace_attrs("  room attr", req->attrs, req->attr_count);
    return submit(ctx_id, WIRE_SET_ROOM_INTERNAL, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_set_room_external(uint16_t ctx_id, const GSetRoomExternalRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SET_ROOM_EXTERNAL, 0);
    wire_put_u64(&b, req->room_id);
    put_int_attrs(&b, req->sint, req->sint_count); put_bin_attrs(&b, req->sbin, req->sbin_count); put_bin_attrs(&b, req->bin, req->bin_count);
    TRACE("sceNpMatching2SetRoomDataExternal(room %llu, %llu ints, %llu sbins, %llu bins)", (unsigned long long)req->room_id,
          (unsigned long long)req->sint_count, (unsigned long long)req->sbin_count, (unsigned long long)req->bin_count);
    trace_int_attrs("  searchable int", req->sint, req->sint_count);
    trace_attrs("  searchable bin", req->sbin, req->sbin_count);
    trace_attrs("  external bin", req->bin, req->bin_count);
    return submit(ctx_id, WIRE_SET_ROOM_EXTERNAL, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_set_member_internal(uint16_t ctx_id, const GSetMemberRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SET_MEMBER, 0);
    wire_put_u64(&b, req->room_id); wire_put_u16(&b, req->member_id); wire_put_u8(&b, req->team_id);
    put_bin_attrs(&b, req->attrs, req->attr_count);
    TRACE("sceNpMatching2SetRoomMemberDataInternal(room %llu, member %u, %llu attrs)", (unsigned long long)req->room_id, req->member_id, (unsigned long long)req->attr_count);
    trace_attrs("  member attr", req->attrs, req->attr_count);
    return submit(ctx_id, WIRE_SET_MEMBER, 0, opt, assigned, req->room_id, req->member_id, &b);
}
static ABI int32_t m2_set_sig_opt(uint16_t ctx_id, const GSetSigOptRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SET_SIGOPT, 0);
    wire_put_u64(&b, req->room_id); wire_put_u8(&b, req->sig_opt.type); wire_put_u8(&b, req->sig_opt.flag); wire_put_u16(&b, req->sig_opt.hub);
    return submit(ctx_id, WIRE_SET_SIGOPT, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_kickout(uint16_t ctx_id, const GKickoutRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_KICK, 0);
    wire_put_u64(&b, req->room_id); wire_put_u16(&b, req->member_id); wire_put_u8(&b, req->block);
    TRACE("sceNpMatching2KickoutRoomMember(room %llu, member %u)", (unsigned long long)req->room_id, req->member_id);
    return submit(ctx_id, WIRE_KICK, 0, opt, assigned, req->room_id, req->member_id, &b);
}
static int32_t send_message(uint16_t ctx_id, const GSendMessageRequest *req, const GRequestOptParam *opt, uint32_t *assigned, int chat) {
    if (!req || (!req->msg && req->msg_len) || req->msg_len > WIRE_MAX_MESSAGE) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_MESSAGE, 0);
    wire_put_u64(&b, req->room_id); wire_put_u8(&b, req->cast_type); wire_put_u8(&b, (uint8_t)chat);
    uint16_t targets = 0;
    if (req->cast_type == WIRE_CAST_UNICAST) { wire_put_u16(&b, 1); wire_put_u16(&b, req->dst.unicast); targets = 1; }
    else if (req->cast_type == WIRE_CAST_MULTICAST) {
        targets = (uint16_t)(req->dst.multicast.count > WIRE_MAX_MEMBERS ? WIRE_MAX_MEMBERS : req->dst.multicast.count);
        wire_put_u16(&b, targets);
        for (uint16_t i = 0; i < targets; ++i) wire_put_u16(&b, req->dst.multicast.ids ? req->dst.multicast.ids[i] : 0);
    } else if (req->cast_type == WIRE_CAST_MULTICAST_TEAM) { wire_put_u16(&b, 1); wire_put_u16(&b, req->dst.team); }
    else wire_put_u16(&b, 0);
    wire_put_blob(&b, req->msg, (uint32_t)req->msg_len);
    TRACE("sceNpMatching2SendRoom%sMessage(room %llu, cast %u, %u targets, %llu bytes)", chat ? "Chat" : "", (unsigned long long)req->room_id, req->cast_type, targets, (unsigned long long)req->msg_len);
    runtime_online_dump("  message out", req->msg, (size_t)req->msg_len);
    return submit(ctx_id, WIRE_MESSAGE, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_send_message(uint16_t ctx_id, const GSendMessageRequest *req, const GRequestOptParam *opt, uint32_t *assigned) { return send_message(ctx_id, req, opt, assigned, 0); }
static ABI int32_t m2_send_chat(uint16_t ctx_id, const GSendMessageRequest *req, const GRequestOptParam *opt, uint32_t *assigned) { return send_message(ctx_id, req, opt, assigned, 1); }
static ABI int32_t m2_get_member(uint16_t ctx_id, const GGetMemberRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_GET_MEMBER, 0);
    wire_put_u64(&b, req->room_id); wire_put_u16(&b, req->member_id);
    return submit(ctx_id, WIRE_GET_MEMBER, 0, opt, assigned, req->room_id, req->member_id, &b);
}
static ABI int32_t m2_get_room_internal(uint16_t ctx_id, const GGetRoomInternalRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_GET_ROOM_INTERNAL, 0);
    wire_put_u64(&b, req->room_id);
    return submit(ctx_id, WIRE_GET_ROOM_INTERNAL, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_get_room_external_list(uint16_t ctx_id, const GGetRoomExternalListRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req || !req->room_ids || !req->room_id_count) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_GET_ROOM_EXTERNAL, 0);
    uint16_t n = (uint16_t)(req->room_id_count > 16 ? 16 : req->room_id_count);
    wire_put_u16(&b, n);
    for (uint16_t i = 0; i < n; ++i) wire_put_u64(&b, req->room_ids[i]);
    return submit(ctx_id, WIRE_GET_ROOM_EXTERNAL, 0, opt, assigned, req->room_ids[0], 0, &b);
}
static ABI int32_t m2_ping(uint16_t ctx_id, const GPingRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_PING, 0);
    wire_put_u64(&b, req->room_id); wire_put_u32(&b, runtime_net_relay_rtt());
    return submit(ctx_id, WIRE_PING, 0, opt, assigned, req->room_id, 0, &b);
}
static ABI int32_t m2_set_user_info(uint16_t ctx_id, const GSetUserInfoRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    if (!req) return M2_ERROR_INVALID_ARGUMENT;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_SET_USER_INFO, 0);
    put_bin_attrs(&b, req->attrs, req->attr_count);
    return submit(ctx_id, WIRE_SET_USER_INFO, 0, opt, assigned, 0, 0, &b);
}
static ABI int32_t m2_get_user_info(uint16_t ctx_id, const GGetUserInfoRequest *req, const GRequestOptParam *opt, uint32_t *assigned) {
    (void)ctx_id; (void)req; (void)opt; (void)assigned;
    TRACE("sceNpMatching2GetUserInfoListA: not supported");
    return M2_ERROR_INVALID_ARGUMENT;
}
/* ---- local queries ---- */
static ABI int32_t m2_member_id_list(uint16_t ctx_id, uint64_t room_id, int32_t sort, uint16_t *ids, uint32_t max) {
    (void)sort;
    if (!ids) return M2_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id); LocalRoom *r = c ? room_of(c, room_id) : NULL;
    int32_t n = 0;
    if (r) for (uint16_t i = 0; i < r->room.member_count && (uint32_t)n < max; ++i) ids[n++] = r->room.members[i].member_id;
    pthread_mutex_unlock(&lock);
    if (!c) return M2_ERROR_CONTEXT_NOT_FOUND;
    if (!r) return M2_ERROR_ROOM_NOT_FOUND;
    return n;
}
static ABI int32_t m2_sig_opt_local(uint16_t ctx_id, uint64_t room_id, GSigOptParam *out) {
    if (!out) return M2_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id); LocalRoom *r = c ? room_of(c, room_id) : NULL;
    if (r) { memset(out, 0, sizeof(*out)); out->type = r->room.sig_type; out->flag = r->room.sig_flag; out->hub = r->room.sig_hub; }
    pthread_mutex_unlock(&lock);
    if (!c) return M2_ERROR_CONTEXT_NOT_FOUND;
    return r ? 0 : M2_ERROR_ROOM_NOT_FOUND;
}
/* SignalingGetConnectionInfoA codes: 1 rtt, 2 bandwidth, 3 peer NpId, 4 peer address, 5 mapped address, 6 packet loss. */
static ABI int32_t m2_sig_connection_info(uint16_t ctx_id, uint64_t room_id, uint16_t member_id, int32_t code, uint8_t *out) {
    if (!out) return M2_ERROR_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id); LocalRoom *r = c ? room_of(c, room_id) : NULL;
    int j = r ? member_index(&r->room, member_id) : -1;
    int32_t result = !c ? M2_ERROR_CONTEXT_NOT_FOUND : !r ? M2_ERROR_ROOM_NOT_FOUND : j < 0 ? M2_ERROR_MEMBER_NOT_FOUND : 0;
    if (!result) {
        const WireMember *m = &r->room.members[j];
        uint32_t rtt = r->sig[j].rtt_us ? r->sig[j].rtt_us : runtime_net_relay_rtt() * 2;
        if (!rtt) rtt = 30000;
        uint16_t port = m->p2p_port ? m->p2p_port : WIRE_DEFAULT_P2P_PORT;
        uint32_t ip = htonl(wire_virtual_ip(m->client_id));
        switch (code) {
        case 1: memcpy(out, &rtt, 4); break;
        case 2: { uint32_t bw = 100000; memcpy(out, &bw, 4); break; }
        case 3: { uint8_t np[36]; memset(np, 0, sizeof(np)); memcpy(np, m->online_id, strnlen(m->online_id, 16)); memcpy(out, np, 36); break; }
        case 4: case 5: { uint16_t p = htons(port); memcpy(out, &ip, 4); memcpy(out + 4, &p, 2); break; }
        case 6: { uint32_t loss = 0; memcpy(out, &loss, 4); break; }
        default: result = M2_ERROR_INVALID_ARGUMENT; break;
        }
        if (!result) TRACE("sceNpMatching2SignalingGetConnectionInfoA(member %u, code %d) -> 10.77.%u.%u:%u rtt %u", member_id, code, m->client_id >> 8, m->client_id & 255, port, rtt);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
static ABI int32_t m2_sig_connection_status(uint16_t ctx_id, uint64_t room_id, uint16_t member_id, int32_t *status, uint32_t *addr, uint16_t *port) {
    pthread_mutex_lock(&lock);
    Context *c = context_of(ctx_id); LocalRoom *r = c ? room_of(c, room_id) : NULL;
    int j = r ? member_index(&r->room, member_id) : -1;
    int32_t result = !c ? M2_ERROR_CONTEXT_NOT_FOUND : !r ? M2_ERROR_ROOM_NOT_FOUND : j < 0 ? M2_ERROR_MEMBER_NOT_FOUND : 0;
    if (!result) {
        if (status) *status = r->sig[j].status == SIG_ESTABLISHED ? 2 : r->sig[j].status == SIG_DEAD ? 0 : 1; /* INACTIVE 0, PENDING 1, ACTIVE 2 */
        if (addr) *addr = htonl(wire_virtual_ip(r->room.members[j].client_id));
        if (port) *port = htons(r->room.members[j].p2p_port ? r->room.members[j].p2p_port : WIRE_DEFAULT_P2P_PORT);
    }
    pthread_mutex_unlock(&lock);
    return result;
}
/* Memory statistics the toolkit's diagnostics ask for: the module uses no guest pool. */
static ABI int32_t m2_memory_info(uint8_t *info) { if (!info) return M2_ERROR_INVALID_ARGUMENT; memset(info, 0, 32); return 0; }

void runtime_matching_report(void) {
    if (stat_requests || stat_events) printf("Runtime: matching requests=%zu, events=%zu, callbacks=%zu\n", stat_requests, stat_events, stat_callbacks);
}
static const RuntimeExport exports[] = {
    {"sceNpMatching2Initialize", m2_initialize}, {"sceNpMatching2Terminate", m2_terminate},
    {"sceNpMatching2CreateContextA", m2_create_context}, {"sceNpMatching2CreateContext", m2_create_context},
    {"sceNpMatching2DestroyContext", m2_destroy_context},
    {"sceNpMatching2ContextStart", m2_context_start}, {"sceNpMatching2ContextStop", m2_context_stop},
    {"sceNpMatching2RegisterContextCallback", m2_register_context_callback},
    {"sceNpMatching2RegisterRoomEventCallback", m2_register_room_event_callback},
    {"sceNpMatching2RegisterRoomMessageCallback", m2_register_room_message_callback},
    {"sceNpMatching2RegisterSignalingCallback", m2_register_signaling_callback},
    {"sceNpMatching2RegisterLobbyEventCallback", m2_set_default_opt},
    {"sceNpMatching2SetDefaultRequestOptParam", m2_set_default_opt},
    {"sceNpMatching2GetServerId", m2_get_server_id}, {"sceNpMatching2GetWorldInfoList", m2_get_world_list},
    {"sceNpMatching2CreateJoinRoomA", m2_create_join_room}, {"sceNpMatching2CreateJoinRoom", m2_create_join_room},
    {"sceNpMatching2JoinRoomA", m2_join_room}, {"sceNpMatching2JoinRoom", m2_join_room},
    {"sceNpMatching2SearchRoom", m2_search_room}, {"sceNpMatching2LeaveRoom", m2_leave_room},
    {"sceNpMatching2SetRoomDataInternal", m2_set_room_internal}, {"sceNpMatching2SetRoomDataExternal", m2_set_room_external},
    {"sceNpMatching2SetRoomMemberDataInternal", m2_set_member_internal}, {"sceNpMatching2SetSignalingOptParam", m2_set_sig_opt},
    {"sceNpMatching2KickoutRoomMember", m2_kickout},
    {"sceNpMatching2SendRoomMessage", m2_send_message}, {"sceNpMatching2SendRoomChatMessage", m2_send_chat},
    {"sceNpMatching2GetRoomMemberDataInternal", m2_get_member}, {"sceNpMatching2GetRoomDataInternal", m2_get_room_internal},
    {"sceNpMatching2GetRoomDataExternalList", m2_get_room_external_list},
    {"sceNpMatching2SignalingGetPingInfo", m2_ping},
    {"sceNpMatching2SetUserInfo", m2_set_user_info}, {"sceNpMatching2GetUserInfoListA", m2_get_user_info},
    {"sceNpMatching2GetRoomMemberIdListLocal", m2_member_id_list}, {"sceNpMatching2GetSignalingOptParamLocal", m2_sig_opt_local},
    {"sceNpMatching2SignalingGetConnectionInfoA", m2_sig_connection_info}, {"sceNpMatching2SignalingGetConnectionInfo", m2_sig_connection_info},
    {"sceNpMatching2SignalingGetConnectionStatus", m2_sig_connection_status},
    {"sceNpMatching2GetMemoryInfo", m2_memory_info}, {"sceNpMatching2GetSslMemoryInfo", m2_memory_info},
};
uintptr_t runtime_matching_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
