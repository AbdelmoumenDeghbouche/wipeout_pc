/* Wire protocol codec shared by the matching client and server (runtime_wire.h). */
#include "runtime_wire.h"
#include <stdlib.h>
#include <string.h>

void wire_writer_init(WireBuf *b) { memset(b, 0, sizeof(*b)); b->owned = 1; }
void wire_reader_init(WireBuf *b, const void *data, size_t size) {
    memset(b, 0, sizeof(*b)); b->data = (uint8_t *)data; b->size = size; b->capacity = size;
}
void wire_free(WireBuf *b) { if (b->owned) free(b->data); memset(b, 0, sizeof(*b)); }
static int reserve(WireBuf *b, size_t extra) {
    if (b->error) return 0;
    if (b->size + extra <= b->capacity) return 1;
    size_t capacity = b->capacity ? b->capacity * 2 : 1024;
    while (capacity < b->size + extra) capacity *= 2;
    if (capacity > WIRE_MAX_FRAME + WIRE_HEADER) { b->error = 1; return 0; }
    uint8_t *data = realloc(b->data, capacity);
    if (!data) { b->error = 1; return 0; }
    b->data = data; b->capacity = capacity; return 1;
}
void wire_put_bytes(WireBuf *b, const void *data, size_t size) {
    if (!reserve(b, size)) return;
    memcpy(b->data + b->size, data, size); b->size += size;
}
void wire_put_u8(WireBuf *b, uint8_t v) { wire_put_bytes(b, &v, 1); }
void wire_put_u16(WireBuf *b, uint16_t v) { uint8_t d[2] = {(uint8_t)v, (uint8_t)(v >> 8)}; wire_put_bytes(b, d, 2); }
void wire_put_u32(WireBuf *b, uint32_t v) { uint8_t d[4]; for (int i = 0; i < 4; ++i) d[i] = (uint8_t)(v >> (8 * i)); wire_put_bytes(b, d, 4); }
void wire_put_u64(WireBuf *b, uint64_t v) { uint8_t d[8]; for (int i = 0; i < 8; ++i) d[i] = (uint8_t)(v >> (8 * i)); wire_put_bytes(b, d, 8); }
void wire_put_blob(WireBuf *b, const void *data, uint32_t size) { wire_put_u32(b, size); if (size) wire_put_bytes(b, data, size); }
void wire_put_string(WireBuf *b, const char *text) {
    size_t n = text ? strlen(text) : 0;
    if (n > 65535) n = 65535;
    wire_put_u16(b, (uint16_t)n); if (n) wire_put_bytes(b, text, n);
}
int wire_get_bytes(WireBuf *b, void *out, size_t size) {
    if (b->error || b->pos + size > b->size) { b->error = 1; if (size) memset(out, 0, size); return 0; }
    memcpy(out, b->data + b->pos, size); b->pos += size; return 1;
}
uint8_t wire_get_u8(WireBuf *b) { uint8_t v = 0; wire_get_bytes(b, &v, 1); return v; }
uint16_t wire_get_u16(WireBuf *b) { uint8_t d[2] = {0, 0}; wire_get_bytes(b, d, 2); return (uint16_t)(d[0] | d[1] << 8); }
uint32_t wire_get_u32(WireBuf *b) { uint8_t d[4] = {0}; wire_get_bytes(b, d, 4); uint32_t v = 0; for (int i = 3; i >= 0; --i) v = v << 8 | d[i]; return v; }
uint64_t wire_get_u64(WireBuf *b) { uint8_t d[8] = {0}; wire_get_bytes(b, d, 8); uint64_t v = 0; for (int i = 7; i >= 0; --i) v = v << 8 | d[i]; return v; }
uint32_t wire_get_blob(WireBuf *b, void *out, uint32_t max) {
    uint32_t size = wire_get_u32(b);
    if (b->error || size > max || b->pos + size > b->size) { b->error = 1; return 0; }
    if (size) { memcpy(out, b->data + b->pos, size); b->pos += size; }
    return size;
}
void wire_get_string(WireBuf *b, char *out, size_t size) {
    uint16_t n = wire_get_u16(b);
    if (b->error || b->pos + n > b->size || !size) { b->error = 1; if (size) out[0] = 0; return; }
    size_t copy = n < size - 1 ? n : size - 1;
    memcpy(out, b->data + b->pos, copy); out[copy] = 0; b->pos += n;
}
void wire_begin(WireBuf *b, uint16_t type, uint32_t token) {
    wire_put_u32(b, 0); wire_put_u16(b, type); wire_put_u16(b, 0); wire_put_u32(b, token);
}
void wire_finish(WireBuf *b) {
    if (b->error || b->size < WIRE_HEADER) return;
    uint32_t length = (uint32_t)(b->size - WIRE_HEADER);
    for (int i = 0; i < 4; ++i) b->data[i] = (uint8_t)(length >> (8 * i));
}
int64_t wire_parse_header(const uint8_t header[WIRE_HEADER], uint16_t *type, uint32_t *token) {
    uint32_t length = 0, t = 0; uint16_t ty = 0;
    for (int i = 3; i >= 0; --i) length = length << 8 | header[i];
    ty = (uint16_t)(header[4] | header[5] << 8);
    for (int i = 11; i >= 8; --i) t = t << 8 | header[i];
    if (length > WIRE_MAX_FRAME) return -1;
    if (type) *type = ty;
    if (token) *token = t;
    return length;
}

void wire_put_bin_list(WireBuf *b, const WireBinList *list) {
    uint16_t count = list->count <= WIRE_MAX_ATTRS ? list->count : WIRE_MAX_ATTRS;
    wire_put_u16(b, count);
    for (uint16_t i = 0; i < count; ++i) {
        wire_put_u16(b, list->items[i].id);
        uint32_t size = list->items[i].size <= WIRE_MAX_ATTR_SIZE ? list->items[i].size : WIRE_MAX_ATTR_SIZE;
        wire_put_blob(b, list->items[i].data, size);
    }
}
void wire_get_bin_list(WireBuf *b, WireBinList *list) {
    memset(list, 0, sizeof(*list));
    uint16_t count = wire_get_u16(b);
    if (count > WIRE_MAX_ATTRS) { b->error = 1; return; }
    for (uint16_t i = 0; i < count; ++i) {
        list->items[i].id = wire_get_u16(b);
        list->items[i].size = wire_get_blob(b, list->items[i].data, WIRE_MAX_ATTR_SIZE);
    }
    list->count = b->error ? 0 : count;
}
void wire_put_int_list(WireBuf *b, const WireIntList *list) {
    uint16_t count = list->count <= WIRE_MAX_ATTRS ? list->count : WIRE_MAX_ATTRS;
    wire_put_u16(b, count);
    for (uint16_t i = 0; i < count; ++i) { wire_put_u16(b, list->items[i].id); wire_put_u32(b, list->items[i].value); }
}
void wire_get_int_list(WireBuf *b, WireIntList *list) {
    memset(list, 0, sizeof(*list));
    uint16_t count = wire_get_u16(b);
    if (count > WIRE_MAX_ATTRS) { b->error = 1; return; }
    for (uint16_t i = 0; i < count; ++i) { list->items[i].id = wire_get_u16(b); list->items[i].value = wire_get_u32(b); }
    list->count = b->error ? 0 : count;
}
void wire_put_member(WireBuf *b, const WireMember *m) {
    wire_put_u16(b, m->member_id); wire_put_u16(b, m->client_id); wire_put_u16(b, m->p2p_port);
    wire_put_u8(b, m->team_id); wire_put_u8(b, m->nat_type);
    wire_put_u32(b, m->flag_attr); wire_put_u32(b, m->platform); wire_put_u32(b, m->rtt_us);
    wire_put_u64(b, m->account_id); wire_put_u64(b, m->join_date);
    wire_put_string(b, m->online_id);
    wire_put_bin_list(b, &m->attrs);
}
void wire_get_member(WireBuf *b, WireMember *m) {
    memset(m, 0, sizeof(*m));
    m->member_id = wire_get_u16(b); m->client_id = wire_get_u16(b); m->p2p_port = wire_get_u16(b);
    m->team_id = wire_get_u8(b); m->nat_type = wire_get_u8(b);
    m->flag_attr = wire_get_u32(b); m->platform = wire_get_u32(b); m->rtt_us = wire_get_u32(b);
    m->account_id = wire_get_u64(b); m->join_date = wire_get_u64(b);
    wire_get_string(b, m->online_id, sizeof(m->online_id));
    wire_get_bin_list(b, &m->attrs);
}
void wire_put_room(WireBuf *b, const WireRoom *r) {
    wire_put_u16(b, r->server_id); wire_put_u16(b, r->owner_id); wire_put_u16(b, r->me_id);
    wire_put_u32(b, r->world_id); wire_put_u32(b, r->max_slot); wire_put_u32(b, r->flag_attr);
    wire_put_u64(b, r->lobby_id); wire_put_u64(b, r->room_id);
    wire_put_u64(b, r->password_slot_mask); wire_put_u64(b, r->joined_slot_mask); wire_put_u64(b, r->password);
    wire_put_u8(b, r->has_password); wire_put_u8(b, r->sig_type); wire_put_u8(b, r->sig_flag); wire_put_u16(b, r->sig_hub);
    wire_put_bin_list(b, &r->bin_internal); wire_put_bin_list(b, &r->bin_external);
    wire_put_bin_list(b, &r->searchable_bin); wire_put_int_list(b, &r->searchable_int);
    uint16_t count = r->member_count <= WIRE_MAX_MEMBERS ? r->member_count : WIRE_MAX_MEMBERS;
    wire_put_u16(b, count);
    for (uint16_t i = 0; i < count; ++i) wire_put_member(b, &r->members[i]);
}
void wire_get_room(WireBuf *b, WireRoom *r) {
    memset(r, 0, sizeof(*r));
    r->server_id = wire_get_u16(b); r->owner_id = wire_get_u16(b); r->me_id = wire_get_u16(b);
    r->world_id = wire_get_u32(b); r->max_slot = wire_get_u32(b); r->flag_attr = wire_get_u32(b);
    r->lobby_id = wire_get_u64(b); r->room_id = wire_get_u64(b);
    r->password_slot_mask = wire_get_u64(b); r->joined_slot_mask = wire_get_u64(b); r->password = wire_get_u64(b);
    r->has_password = wire_get_u8(b); r->sig_type = wire_get_u8(b); r->sig_flag = wire_get_u8(b); r->sig_hub = wire_get_u16(b);
    wire_get_bin_list(b, &r->bin_internal); wire_get_bin_list(b, &r->bin_external);
    wire_get_bin_list(b, &r->searchable_bin); wire_get_int_list(b, &r->searchable_int);
    uint16_t count = wire_get_u16(b);
    if (count > WIRE_MAX_MEMBERS) { b->error = 1; return; }
    for (uint16_t i = 0; i < count; ++i) wire_get_member(b, &r->members[i]);
    r->member_count = b->error ? 0 : count;
}
void wire_put_search(WireBuf *b, const WireSearch *s) {
    wire_put_u32(b, s->world_id); wire_put_u32(b, s->start_index); wire_put_u32(b, s->max);
    wire_put_u32(b, s->flag_filter); wire_put_u32(b, s->flag_attr);
    uint16_t n = s->int_filter_count <= WIRE_MAX_ATTRS ? s->int_filter_count : WIRE_MAX_ATTRS;
    wire_put_u16(b, n);
    for (uint16_t i = 0; i < n; ++i) {
        wire_put_u8(b, s->int_filters[i].op); wire_put_u16(b, s->int_filters[i].attr.id); wire_put_u32(b, s->int_filters[i].attr.value);
    }
    n = s->bin_filter_count <= WIRE_MAX_ATTRS ? s->bin_filter_count : WIRE_MAX_ATTRS;
    wire_put_u16(b, n);
    for (uint16_t i = 0; i < n; ++i) {
        wire_put_u8(b, s->bin_filters[i].op); wire_put_u16(b, s->bin_filters[i].attr.id);
        uint32_t size = s->bin_filters[i].attr.size <= WIRE_MAX_ATTR_SIZE ? s->bin_filters[i].attr.size : WIRE_MAX_ATTR_SIZE;
        wire_put_blob(b, s->bin_filters[i].attr.data, size);
    }
}
void wire_get_search(WireBuf *b, WireSearch *s) {
    memset(s, 0, sizeof(*s));
    s->world_id = wire_get_u32(b); s->start_index = wire_get_u32(b); s->max = wire_get_u32(b);
    s->flag_filter = wire_get_u32(b); s->flag_attr = wire_get_u32(b);
    uint16_t n = wire_get_u16(b);
    if (n > WIRE_MAX_ATTRS) { b->error = 1; return; }
    for (uint16_t i = 0; i < n; ++i) {
        s->int_filters[i].op = wire_get_u8(b); s->int_filters[i].attr.id = wire_get_u16(b); s->int_filters[i].attr.value = wire_get_u32(b);
    }
    s->int_filter_count = n;
    n = wire_get_u16(b);
    if (n > WIRE_MAX_ATTRS) { b->error = 1; return; }
    for (uint16_t i = 0; i < n; ++i) {
        s->bin_filters[i].op = wire_get_u8(b); s->bin_filters[i].attr.id = wire_get_u16(b);
        s->bin_filters[i].attr.size = wire_get_blob(b, s->bin_filters[i].attr.data, WIRE_MAX_ATTR_SIZE);
    }
    s->bin_filter_count = n;
    if (b->error) memset(s, 0, sizeof(*s));
}

WireBin *wire_bin_find(WireBinList *list, uint16_t id) {
    for (uint16_t i = 0; i < list->count && i < WIRE_MAX_ATTRS; ++i) if (list->items[i].id == id) return &list->items[i];
    return NULL;
}
int wire_bin_set(WireBinList *list, uint16_t id, const void *data, uint32_t size) {
    if (size > WIRE_MAX_ATTR_SIZE) size = WIRE_MAX_ATTR_SIZE;
    WireBin *item = wire_bin_find(list, id);
    if (!item) {
        if (list->count >= WIRE_MAX_ATTRS) return -1;
        item = &list->items[list->count++];
        item->id = id;
    }
    item->size = size;
    memset(item->data, 0, sizeof(item->data));
    if (size && data) memcpy(item->data, data, size);
    return 0;
}
WireInt *wire_int_find(WireIntList *list, uint16_t id) {
    for (uint16_t i = 0; i < list->count && i < WIRE_MAX_ATTRS; ++i) if (list->items[i].id == id) return &list->items[i];
    return NULL;
}
int wire_int_set(WireIntList *list, uint16_t id, uint32_t value) {
    WireInt *item = wire_int_find(list, id);
    if (!item) {
        if (list->count >= WIRE_MAX_ATTRS) return -1;
        item = &list->items[list->count++];
        item->id = id;
    }
    item->value = value;
    return 0;
}
static int compare(uint8_t op, int64_t a, int64_t b) {
    switch (op) {
    case WIRE_OP_EQ: return a == b; case WIRE_OP_NE: return a != b; case WIRE_OP_LT: return a < b;
    case WIRE_OP_LE: return a <= b; case WIRE_OP_GT: return a > b; case WIRE_OP_GE: return a >= b;
    default: return 1;
    }
}
int wire_room_matches(const WireRoom *room, const WireSearch *search) {
    if (search->world_id && room->world_id != search->world_id) return 0;
    if ((room->flag_attr & search->flag_filter) != (search->flag_attr & search->flag_filter)) return 0;
    for (uint16_t i = 0; i < search->int_filter_count && i < WIRE_MAX_ATTRS; ++i) {
        const WireInt *attr = wire_int_find((WireIntList *)&room->searchable_int, search->int_filters[i].attr.id);
        if (!compare(search->int_filters[i].op, attr ? attr->value : 0, search->int_filters[i].attr.value)) return 0;
    }
    for (uint16_t i = 0; i < search->bin_filter_count && i < WIRE_MAX_ATTRS; ++i) {
        const WireBin *attr = wire_bin_find((WireBinList *)&room->searchable_bin, search->bin_filters[i].attr.id);
        const WireBinFilter *f = &search->bin_filters[i];
        int equal = attr && attr->size == f->attr.size && !memcmp(attr->data, f->attr.data, attr->size);
        if (f->op == WIRE_OP_NE ? equal : !equal) return 0;
    }
    return 1;
}
void wire_relay_pack(uint8_t out[WIRE_RELAY_HEADER], const WireRelayHeader *h) {
    const uint16_t words[7] = {h->magic, h->src_client, h->dst_client, h->src_port, h->dst_port, h->src_vport, h->dst_vport};
    for (int i = 0; i < 7; ++i) { out[2 * i] = (uint8_t)words[i]; out[2 * i + 1] = (uint8_t)(words[i] >> 8); }
    out[14] = h->kind; out[15] = h->version;
}
int wire_relay_unpack(const uint8_t *data, size_t size, WireRelayHeader *h) {
    if (size < WIRE_RELAY_HEADER) return 0;
    uint16_t words[7];
    for (int i = 0; i < 7; ++i) words[i] = (uint16_t)(data[2 * i] | data[2 * i + 1] << 8);
    h->magic = words[0]; h->src_client = words[1]; h->dst_client = words[2]; h->src_port = words[3];
    h->dst_port = words[4]; h->src_vport = words[5]; h->dst_vport = words[6]; h->kind = data[14]; h->version = data[15];
    return h->magic == WIRE_RELAY_MAGIC && h->version == WIRE_VERSION;
}
