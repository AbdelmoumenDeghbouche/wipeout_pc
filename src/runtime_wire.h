/* Wire protocol between the game runtime (sceNpMatching2 client, runtime_matching.c) and the
 * match server (runtime_matchserver.c, hosted by a player or run as gow3-server). Both sides
 * are this project's code, so the format is private: little-endian, framed messages over one
 * TCP connection per matching context, and a UDP relay for the game's peer-to-peer packets.
 *
 * Frame: u32 payload length, u16 type, u16 reserved, u32 token (request id echoed in the
 * answer; 0 in events), then the payload. Strings are u16 length + bytes (no terminator). */
#ifndef GOW3_RUNTIME_WIRE_H
#define GOW3_RUNTIME_WIRE_H
#include <stdint.h>
#include <stddef.h>

#define WIRE_VERSION 1
#define WIRE_DEFAULT_PORT 7788          /* TCP matching and UDP relay share the number */
#define WIRE_HEADER 12
#define WIRE_MAX_FRAME (256 * 1024)
#define WIRE_ONLINE_ID 16
#define WIRE_MAX_MEMBERS 16
#define WIRE_MAX_ATTRS 8
#define WIRE_MAX_ATTR_SIZE 1024
#define WIRE_MAX_ROOMS 256
#define WIRE_MAX_CLIENTS 256
#define WIRE_MAX_MESSAGE 8192
#define WIRE_SERVER_ID 1
#define WIRE_WORLD_ID 1
#define WIRE_WORLD_COUNT 16
/* The virtual LAN the game sees: every client is 10.77.<id>>8>.<id&255>. */
#define WIRE_VIRTUAL_NET 0x0A4D0000u    /* 10.77.0.0 */
#define WIRE_VIRTUAL_MASK 0xFFFF0000u
#define WIRE_DEFAULT_P2P_PORT 3658      /* the game's own UDP port (sceNetBind of its P2P socket) */
/* UDP relay packets: 16-byte header then the game's datagram. */
#define WIRE_RELAY_MAGIC 0x5057         /* "WP" */
#define WIRE_RELAY_DATA 0
#define WIRE_RELAY_REGISTER 1           /* src_client + session token: tells the server the endpoint */
#define WIRE_RELAY_HEADER 16

typedef enum {
    WIRE_HELLO = 1, WIRE_WELCOME, WIRE_OK, WIRE_ERROR,
    WIRE_GET_WORLDS, WIRE_WORLDS,
    WIRE_CREATE_ROOM, WIRE_JOIN_ROOM, WIRE_ROOM, WIRE_LEAVE_ROOM,
    WIRE_SEARCH, WIRE_ROOM_LIST, WIRE_GET_ROOM_EXTERNAL, WIRE_GET_ROOM_INTERNAL,
    WIRE_GET_MEMBER, WIRE_MEMBER,
    WIRE_SET_ROOM_INTERNAL, WIRE_SET_ROOM_EXTERNAL, WIRE_SET_MEMBER, WIRE_SET_SIGOPT,
    WIRE_KICK, WIRE_MESSAGE, WIRE_PING, WIRE_PONG, WIRE_SET_USER_INFO, WIRE_GET_USER_INFO, WIRE_USER_INFO,
    WIRE_P2P_PORT,
    WIRE_EV_JOINED = 0x100, WIRE_EV_LEFT, WIRE_EV_KICKED, WIRE_EV_DESTROYED, WIRE_EV_OWNER,
    WIRE_EV_ROOM_UPDATED, WIRE_EV_MEMBER_UPDATED, WIRE_EV_MESSAGE, WIRE_EV_SIGNALING,
} WireType;

/* Error codes carried by WIRE_ERROR: the PS4 values the game expects back. */
#define WIRE_ERR_NO_SUCH_ROOM        ((int32_t)0x80550D0C)
#define WIRE_ERR_ROOM_FULL           ((int32_t)0x80550D08)
#define WIRE_ERR_PASSWORD_MISMATCH   ((int32_t)0x80550D09)
#define WIRE_ERR_ALREADY_JOINED      ((int32_t)0x80550D0E)
#define WIRE_ERR_NOT_IN_ROOM         ((int32_t)0x80550D0B)
#define WIRE_ERR_NOT_OWNER           ((int32_t)0x80550D14)
#define WIRE_ERR_NO_SUCH_MEMBER      ((int32_t)0x80550D11)
#define WIRE_ERR_INVALID_ARGUMENT    ((int32_t)0x80550C03)
#define WIRE_ERR_SERVER_UNAVAILABLE  ((int32_t)0x80550D01)
#define WIRE_ERR_NAME_IN_USE         ((int32_t)0x80550D01)

/* Room flag attributes (SceNpMatching2FlagAttr). */
#define WIRE_FLAG_OWNER_AUTO_GRANT 0x80000000u
#define WIRE_FLAG_CLOSED 0x40000000u
#define WIRE_FLAG_FULL 0x20000000u
#define WIRE_FLAG_HIDDEN 0x10000000u
#define WIRE_FLAG_NAT_TYPE_RESTRICTION 0x04000000u
#define WIRE_FLAG_PROHIBITIVE_MODE 0x02000000u
/* Member flag attribute: the room owner. */
#define WIRE_MEMBER_FLAG_OWNER 0x80000000u
/* Event causes (SceNpMatching2EventCause). */
#define WIRE_CAUSE_LEAVE_ACTION 1
#define WIRE_CAUSE_KICKOUT_ACTION 2
#define WIRE_CAUSE_GRANT_OWNER_ACTION 3
#define WIRE_CAUSE_SERVER_OPERATION 4
#define WIRE_CAUSE_MEMBER_DISAPPEARED 5
#define WIRE_CAUSE_SERVER_INTERNAL 6
#define WIRE_CAUSE_CONNECTION_FAILURE 7
#define WIRE_CAUSE_NP_SIGNED_OUT 8
#define WIRE_CAUSE_SYSTEM_ERROR 9
#define WIRE_CAUSE_CONTEXT_ERROR 10
#define WIRE_CAUSE_CONTEXT_ACTION 11
/* Message cast types. */
#define WIRE_CAST_BROADCAST 1
#define WIRE_CAST_UNICAST 2
#define WIRE_CAST_MULTICAST 3
#define WIRE_CAST_MULTICAST_TEAM 4
/* Search filter operators (SceNpMatching2Operator). */
#define WIRE_OP_EQ 1
#define WIRE_OP_NE 2
#define WIRE_OP_LT 3
#define WIRE_OP_LE 4
#define WIRE_OP_GT 5
#define WIRE_OP_GE 6

typedef struct { uint16_t id; uint32_t size; uint8_t data[WIRE_MAX_ATTR_SIZE]; } WireBin;
typedef struct { uint16_t count; WireBin items[WIRE_MAX_ATTRS]; } WireBinList;
typedef struct { uint16_t id; uint32_t value; } WireInt;
typedef struct { uint16_t count; WireInt items[WIRE_MAX_ATTRS]; } WireIntList;
typedef struct { uint8_t op; WireInt attr; } WireIntFilter;
typedef struct { uint8_t op; WireBin attr; } WireBinFilter;

typedef struct {
    uint16_t member_id, client_id, p2p_port;
    uint8_t team_id, nat_type;
    uint32_t flag_attr, platform, rtt_us;
    uint64_t account_id, join_date;
    char online_id[WIRE_ONLINE_ID + 1];
    WireBinList attrs;                       /* room member bin attr internal */
} WireMember;

typedef struct {
    uint16_t server_id, owner_id, me_id, member_count;
    uint32_t world_id, max_slot, flag_attr;
    uint64_t lobby_id, room_id, password_slot_mask, joined_slot_mask, password;
    uint8_t has_password, sig_type, sig_flag;
    uint16_t sig_hub;
    WireBinList bin_internal, bin_external, searchable_bin;
    WireIntList searchable_int;
    WireMember members[WIRE_MAX_MEMBERS];
} WireRoom;

typedef struct {
    uint32_t world_id, start_index, max, flag_filter, flag_attr;
    uint16_t int_filter_count, bin_filter_count;
    WireIntFilter int_filters[WIRE_MAX_ATTRS];
    WireBinFilter bin_filters[WIRE_MAX_ATTRS];
} WireSearch;

/* Growable writer / bounded reader. A reader past its end sets error and returns zeros. */
typedef struct { uint8_t *data; size_t size, capacity, pos; int error, owned; } WireBuf;
void wire_writer_init(WireBuf *b);
void wire_reader_init(WireBuf *b, const void *data, size_t size);
void wire_free(WireBuf *b);
void wire_put_u8(WireBuf *b, uint8_t v);
void wire_put_u16(WireBuf *b, uint16_t v);
void wire_put_u32(WireBuf *b, uint32_t v);
void wire_put_u64(WireBuf *b, uint64_t v);
void wire_put_bytes(WireBuf *b, const void *data, size_t size);
void wire_put_blob(WireBuf *b, const void *data, uint32_t size);   /* u32 size + bytes */
void wire_put_string(WireBuf *b, const char *text);                /* u16 size + bytes */
uint8_t wire_get_u8(WireBuf *b);
uint16_t wire_get_u16(WireBuf *b);
uint32_t wire_get_u32(WireBuf *b);
uint64_t wire_get_u64(WireBuf *b);
int wire_get_bytes(WireBuf *b, void *out, size_t size);
/* Reads a blob of at most `max` bytes into out; returns its size (0 with error on overflow). */
uint32_t wire_get_blob(WireBuf *b, void *out, uint32_t max);
void wire_get_string(WireBuf *b, char *out, size_t size);
/* Frame helpers: the writer gets a header first; wire_finish patches the length. */
void wire_begin(WireBuf *b, uint16_t type, uint32_t token);
void wire_finish(WireBuf *b);
/* Parses a header: returns the payload length, or -1 when the header is invalid. */
int64_t wire_parse_header(const uint8_t header[WIRE_HEADER], uint16_t *type, uint32_t *token);

void wire_put_bin_list(WireBuf *b, const WireBinList *list);
void wire_get_bin_list(WireBuf *b, WireBinList *list);
void wire_put_int_list(WireBuf *b, const WireIntList *list);
void wire_get_int_list(WireBuf *b, WireIntList *list);
void wire_put_member(WireBuf *b, const WireMember *m);
void wire_get_member(WireBuf *b, WireMember *m);
void wire_put_room(WireBuf *b, const WireRoom *r);
void wire_get_room(WireBuf *b, WireRoom *r);
void wire_put_search(WireBuf *b, const WireSearch *s);
void wire_get_search(WireBuf *b, WireSearch *s);

/* Attribute list helpers shared by both sides. */
WireBin *wire_bin_find(WireBinList *list, uint16_t id);
int wire_bin_set(WireBinList *list, uint16_t id, const void *data, uint32_t size);
WireInt *wire_int_find(WireIntList *list, uint16_t id);
int wire_int_set(WireIntList *list, uint16_t id, uint32_t value);
/* Whether a room matches a search (filters on searchable attributes and flags). */
int wire_room_matches(const WireRoom *room, const WireSearch *search);
/* The virtual address of a client, host byte order. */
static inline uint32_t wire_virtual_ip(uint16_t client_id) { return WIRE_VIRTUAL_NET | client_id; }
static inline int wire_is_virtual_ip(uint32_t ip) { return (ip & WIRE_VIRTUAL_MASK) == WIRE_VIRTUAL_NET; }
/* UDP relay header, host byte order fields packed little-endian. */
typedef struct {
    uint16_t magic, src_client, dst_client, src_port, dst_port, src_vport, dst_vport;
    uint8_t kind, version;
} WireRelayHeader;
void wire_relay_pack(uint8_t out[WIRE_RELAY_HEADER], const WireRelayHeader *h);
int wire_relay_unpack(const uint8_t *data, size_t size, WireRelayHeader *h);
#endif
