/* The match server and its wire protocol: two clients create and join a room, see each other's
 * events, exchange a message and a relayed datagram, and the room follows them out. */
#undef NDEBUG
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define closesocket close
#endif
#include "../src/runtime_wire.h"
#include "../src/runtime_matchserver.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT 17788
typedef struct { SOCKET sock, udp; uint16_t client_id; uint64_t session; uint16_t udp_port; uint32_t next_token; } TestClient;

static void send_all(SOCKET s, WireBuf *b) {
    wire_finish(b);
    assert(!b->error);
    size_t done = 0;
    while (done < b->size) { int n = send(s, (const char *)b->data + done, (int)(b->size - done), 0); assert(n > 0); done += (size_t)n; }
    wire_free(b);
}
/* Reads one frame; returns its type and leaves the payload in *out (malloc). */
static uint16_t read_frame(SOCKET s, uint32_t *token, uint8_t **out, size_t *size) {
    uint8_t header[WIRE_HEADER]; size_t got = 0;
    while (got < WIRE_HEADER) { int n = recv(s, (char *)header + got, (int)(WIRE_HEADER - got), 0); assert(n > 0); got += (size_t)n; }
    uint16_t type; int64_t length = wire_parse_header(header, &type, token);
    assert(length >= 0);
    *out = malloc((size_t)length + 1); got = 0;
    while (got < (size_t)length) { int n = recv(s, (char *)*out + got, (int)((size_t)length - got), 0); assert(n > 0); got += (size_t)n; }
    *size = (size_t)length;
    return type;
}
static uint16_t expect(SOCKET s, uint16_t wanted, uint32_t *token, WireBuf *in, uint8_t **owned) {
    uint8_t *payload; size_t size;
    uint16_t type = read_frame(s, token, &payload, &size);
    if (type != wanted) { fprintf(stderr, "expected frame %u, got %u\n", wanted, type); assert(0); }
    wire_reader_init(in, payload, size); *owned = payload;
    return type;
}
static void connect_client(TestClient *c, const char *name, uint64_t account, uint16_t wanted_id) {
    c->sock = socket(AF_INET, SOCK_STREAM, 0); assert(c->sock != INVALID_SOCKET);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(PORT);
    assert(!connect(c->sock, (struct sockaddr *)&addr, sizeof(addr)));
    c->next_token = 1;
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_HELLO, c->next_token++);
    wire_put_u32(&b, WIRE_VERSION); wire_put_u64(&b, account); wire_put_string(&b, name); wire_put_u32(&b, 3);
    wire_put_u16(&b, wanted_id); wire_put_u16(&b, 3658);
    send_all(c->sock, &b);
    uint32_t token; WireBuf in; uint8_t *payload;
    expect(c->sock, WIRE_WELCOME, &token, &in, &payload);
    assert(wire_get_u32(&in) == WIRE_VERSION);
    c->client_id = wire_get_u16(&in); c->session = wire_get_u64(&in); c->udp_port = wire_get_u16(&in);
    assert(c->client_id == wanted_id && c->udp_port == PORT);
    free(payload);
    /* the relay learns this client's endpoint from a register packet */
    c->udp = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in any; memset(&any, 0, sizeof(any)); any.sin_family = AF_INET;
    assert(!bind(c->udp, (struct sockaddr *)&any, sizeof(any)));
    WireRelayHeader h = {WIRE_RELAY_MAGIC, c->client_id, 0, 0, 0, 0, 0, WIRE_RELAY_REGISTER, WIRE_VERSION};
    uint8_t packet[WIRE_RELAY_HEADER + 14]; memset(packet, 0, sizeof(packet));
    wire_relay_pack(packet, &h); memcpy(packet + WIRE_RELAY_HEADER, &c->session, 8);
    uint16_t port = 3658; memcpy(packet + WIRE_RELAY_HEADER + 8, &port, 2);
    uint32_t rtt = 5000; memcpy(packet + WIRE_RELAY_HEADER + 10, &rtt, 4);
    assert(sendto(c->udp, (const char *)packet, sizeof(packet), 0, (struct sockaddr *)&addr, sizeof(addr)) == (int)sizeof(packet));
    uint8_t echo[64]; int n = recv(c->udp, (char *)echo, sizeof(echo), 0);
    assert(n == (int)sizeof(packet) && !memcmp(echo, packet, sizeof(packet)));
}

int main(void) {
#ifdef _WIN32
    WSADATA data; WSAStartup(MAKEWORD(2, 2), &data);
#endif
    /* codec round trip */
    WireRoom room; memset(&room, 0, sizeof(room));
    room.room_id = 42; room.max_slot = 8; room.member_count = 1; room.members[0].member_id = 7;
    snprintf(room.members[0].online_id, sizeof(room.members[0].online_id), "racer");
    wire_bin_set(&room.bin_internal, 0x57, "abc", 3); wire_int_set(&room.searchable_int, 0x4C, 5);
    WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_ROOM, 9); wire_put_room(&b, &room); wire_finish(&b);
    uint16_t type; uint32_t token;
    assert(wire_parse_header(b.data, &type, &token) == (int64_t)(b.size - WIRE_HEADER) && type == WIRE_ROOM && token == 9);
    WireBuf in; wire_reader_init(&in, b.data + WIRE_HEADER, b.size - WIRE_HEADER);
    WireRoom back; wire_get_room(&in, &back);
    assert(!in.error && back.room_id == 42 && back.member_count == 1 && back.members[0].member_id == 7);
    assert(!strcmp(back.members[0].online_id, "racer") && back.bin_internal.count == 1 && back.bin_internal.items[0].size == 3);
    assert(wire_int_find(&back.searchable_int, 0x4C)->value == 5);
    wire_free(&b);
    WireSearch search; memset(&search, 0, sizeof(search));
    search.int_filter_count = 1; search.int_filters[0] = (WireIntFilter){WIRE_OP_EQ, {0x4C, 5}};
    assert(wire_room_matches(&room, &search));
    search.int_filters[0].attr.value = 6;
    assert(!wire_room_matches(&room, &search));

    runtime_matchserver_set_quiet(1);
    assert(runtime_matchserver_start(PORT) == 0);
    TestClient a, c2;
    connect_client(&a, "alice", 1001, 0x0101);
    connect_client(&c2, "bob", 1002, 0x0202);
    { /* the same name from another machine (another loopback address) is refused */
        SOCKET s = socket(AF_INET, SOCK_STREAM, 0); assert(s != INVALID_SOCKET);
        struct sockaddr_in from; memset(&from, 0, sizeof(from));
        from.sin_family = AF_INET; from.sin_addr.s_addr = htonl(INADDR_LOOPBACK + 1);
        assert(!bind(s, (struct sockaddr *)&from, sizeof(from)));
        struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons(PORT);
        assert(!connect(s, (struct sockaddr *)&addr, sizeof(addr)));
        WireBuf b; wire_writer_init(&b); wire_begin(&b, WIRE_HELLO, 1);
        wire_put_u32(&b, WIRE_VERSION); wire_put_u64(&b, 1001); wire_put_string(&b, "alice"); wire_put_u32(&b, 3);
        wire_put_u16(&b, 0x0303); wire_put_u16(&b, 3658);
        send_all(s, &b);
        uint32_t token; WireBuf in; uint8_t *payload;
        expect(s, WIRE_ERROR, &token, &in, &payload);
        assert((int32_t)wire_get_u32(&in) == WIRE_ERR_NAME_IN_USE);
        free(payload); closesocket(s);
    }

    /* alice creates a room with attributes and a member attribute */
    wire_writer_init(&b); wire_begin(&b, WIRE_CREATE_ROOM, a.next_token++);
    wire_put_u32(&b, WIRE_WORLD_ID); wire_put_u32(&b, 8); wire_put_u32(&b, WIRE_FLAG_OWNER_AUTO_GRANT); wire_put_u64(&b, 0);
    wire_put_u8(&b, 0); wire_put_u64(&b, 0); wire_put_u64(&b, 0); wire_put_u8(&b, 1); wire_put_u8(&b, 0); wire_put_u16(&b, 0);
    WireBinList bins; memset(&bins, 0, sizeof(bins)); wire_bin_set(&bins, 0x57, "internal", 8);
    WireIntList ints; memset(&ints, 0, sizeof(ints)); wire_int_set(&ints, 0x4C, 3);
    WireBinList none; memset(&none, 0, sizeof(none));
    WireBinList member; memset(&member, 0, sizeof(member)); wire_bin_set(&member, 0x59, "me", 2);
    wire_put_bin_list(&b, &bins); wire_put_bin_list(&b, &none); wire_put_bin_list(&b, &none); wire_put_int_list(&b, &ints); wire_put_bin_list(&b, &member);
    send_all(a.sock, &b);
    uint8_t *payload;
    expect(a.sock, WIRE_ROOM, &token, &in, &payload);
    wire_get_room(&in, &room); free(payload);
    assert(!in.error && room.member_count == 1 && room.owner_id == room.me_id && room.max_slot == 8);
    assert(room.members[0].client_id == 0x0101 && !strcmp(room.members[0].online_id, "alice"));
    assert(room.members[0].attrs.count == 1 && room.members[0].attrs.items[0].id == 0x59);
    uint64_t room_id = room.room_id;

    /* bob finds it and joins */
    memset(&search, 0, sizeof(search)); search.flag_filter = WIRE_FLAG_FULL | WIRE_FLAG_HIDDEN | WIRE_FLAG_CLOSED;
    wire_writer_init(&b); wire_begin(&b, WIRE_SEARCH, c2.next_token++); wire_put_search(&b, &search); send_all(c2.sock, &b);
    expect(c2.sock, WIRE_ROOM_LIST, &token, &in, &payload);
    assert(wire_get_u32(&in) == 0 && wire_get_u32(&in) == 1 && wire_get_u32(&in) == 1);
    wire_get_room(&in, &room); free(payload);
    assert(room.room_id == room_id && wire_int_find(&room.searchable_int, 0x4C)->value == 3);
    wire_writer_init(&b); wire_begin(&b, WIRE_JOIN_ROOM, c2.next_token++);
    wire_put_u64(&b, room_id); wire_put_u8(&b, 0); wire_put_u64(&b, 0); wire_put_bin_list(&b, &none); send_all(c2.sock, &b);
    expect(c2.sock, WIRE_ROOM, &token, &in, &payload);
    wire_get_room(&in, &room); free(payload);
    assert(room.member_count == 2 && room.me_id != room.owner_id);
    uint16_t bob_id = room.me_id;
    /* bob hears signaling for alice; alice hears bob joined, then signaling for bob */
    expect(c2.sock, WIRE_EV_SIGNALING, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id && wire_get_u16(&in) == room.owner_id && wire_get_u8(&in) == 1 && wire_get_u16(&in) == 0x0101); free(payload);
    expect(a.sock, WIRE_EV_JOINED, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id);
    WireMember m; wire_get_member(&in, &m); free(payload);
    assert(m.member_id == bob_id && !strcmp(m.online_id, "bob"));
    expect(a.sock, WIRE_EV_SIGNALING, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id && wire_get_u16(&in) == bob_id); free(payload);

    /* a relayed datagram from bob to alice's virtual address */
    struct sockaddr_in server; memset(&server, 0, sizeof(server));
    server.sin_family = AF_INET; server.sin_addr.s_addr = htonl(INADDR_LOOPBACK); server.sin_port = htons(PORT);
    WireRelayHeader h = {WIRE_RELAY_MAGIC, c2.client_id, a.client_id, 3658, 3658, 0, 0, WIRE_RELAY_DATA, WIRE_VERSION};
    uint8_t packet[WIRE_RELAY_HEADER + 5]; wire_relay_pack(packet, &h); memcpy(packet + WIRE_RELAY_HEADER, "hello", 5);
    assert(sendto(c2.udp, (const char *)packet, sizeof(packet), 0, (struct sockaddr *)&server, sizeof(server)) == (int)sizeof(packet));
    uint8_t got[64]; int n = recv(a.udp, (char *)got, sizeof(got), 0);
    assert(n == (int)sizeof(packet) && !memcmp(got + WIRE_RELAY_HEADER, "hello", 5));
    WireRelayHeader back_h; assert(wire_relay_unpack(got, (size_t)n, &back_h) && back_h.src_client == c2.client_id && back_h.dst_port == 3658);

    /* a chat message and a member update */
    wire_writer_init(&b); wire_begin(&b, WIRE_MESSAGE, c2.next_token++);
    wire_put_u64(&b, room_id); wire_put_u8(&b, WIRE_CAST_BROADCAST); wire_put_u8(&b, 1); wire_put_u16(&b, 0); wire_put_blob(&b, "hi", 3);
    send_all(c2.sock, &b);
    expect(c2.sock, WIRE_OK, &token, &in, &payload); free(payload);
    expect(a.sock, WIRE_EV_MESSAGE, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id && wire_get_u16(&in) == bob_id && wire_get_u8(&in) == WIRE_CAST_BROADCAST && wire_get_u8(&in) == 1);
    char text[8]; assert(wire_get_blob(&in, text, sizeof(text)) == 3 && !strcmp(text, "hi")); free(payload);
    wire_writer_init(&b); wire_begin(&b, WIRE_SET_MEMBER, c2.next_token++);
    wire_put_u64(&b, room_id); wire_put_u16(&b, bob_id); wire_put_u8(&b, 1); wire_put_bin_list(&b, &member); send_all(c2.sock, &b);
    expect(c2.sock, WIRE_OK, &token, &in, &payload); free(payload);
    expect(c2.sock, WIRE_EV_MEMBER_UPDATED, &token, &in, &payload); free(payload);
    expect(a.sock, WIRE_EV_MEMBER_UPDATED, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id); wire_get_member(&in, &m); free(payload);
    assert(m.member_id == bob_id && m.team_id == 1 && m.attrs.count == 1);

    /* alice (the owner) leaves: bob becomes the owner */
    wire_writer_init(&b); wire_begin(&b, WIRE_LEAVE_ROOM, a.next_token++); wire_put_u64(&b, room_id); send_all(a.sock, &b);
    expect(a.sock, WIRE_OK, &token, &in, &payload); free(payload);
    expect(c2.sock, WIRE_EV_LEFT, &token, &in, &payload); free(payload);
    expect(c2.sock, WIRE_EV_OWNER, &token, &in, &payload);
    assert(wire_get_u64(&in) == room_id); (void)wire_get_u16(&in); assert(wire_get_u16(&in) == bob_id); free(payload);
    /* bob disconnects: the room is gone */
    closesocket(c2.sock);
    wire_writer_init(&b); wire_begin(&b, WIRE_SEARCH, a.next_token++); memset(&search, 0, sizeof(search)); wire_put_search(&b, &search); send_all(a.sock, &b);
    for (int tries = 0; tries < 50; ++tries) {
        expect(a.sock, WIRE_ROOM_LIST, &token, &in, &payload);
        uint32_t total = (wire_get_u32(&in), wire_get_u32(&in)); free(payload);
        if (!total) break;
        assert(tries < 49);
        wire_writer_init(&b); wire_begin(&b, WIRE_SEARCH, a.next_token++); wire_put_search(&b, &search); send_all(a.sock, &b);
    }
    closesocket(a.sock);
    runtime_matchserver_stop();
    puts("matching test: ok");
    return 0;
}
