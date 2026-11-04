/* libSceNpWebApi: the PSN web API, answered here instead of by Sony's servers.
 *
 * The game's toolkit (libSceNpToolkit2) binds every room to a PSN "session" (the console's
 * Sessions tile, invitations): it creates one through the sessionInvitation API before it
 * creates the room, and updates or deletes it later. This module plays that service: the
 * requests are read, a plausible JSON answer is written, nothing leaves the PC. Other API
 * groups answer 404. WIPEOUT_NET_TRACE=1 prints every request and answer. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

#define WEBAPI_OK 0
#define WEBAPI_INVALID_ARGUMENT ((int32_t)0x80552E05)
#define WEBAPI_NOT_INITIALIZED ((int32_t)0x80552E02)
#define WEBAPI_NO_SUCH_REQUEST ((int32_t)0x80552E0A)
#define WEBAPI_NOT_SIGNED_IN ((int32_t)0x80552E07)
#define TRACE(...) runtime_online_log(__VA_ARGS__)
#define MAX_REQUESTS 16
#define MAX_SESSIONS 8

typedef struct {
    int used; int64_t id; int32_t ctx, method, status;
    char group[48], path[256];
    char *body; size_t body_size, body_read;
    char error_object[256];
} Request;
typedef struct { int used; char id[40]; char data[8192]; size_t data_size; } Session;

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static Request requests[MAX_REQUESTS];
static Session sessions[MAX_SESSIONS];
static int next_id = 1;
static int64_t next_request = 1;
static int new_id(void) { pthread_mutex_lock(&lock); int id = next_id++; pthread_mutex_unlock(&lock); return id; }
static const char *method_name(int32_t method) {
    static const char *const names[] = {"GET", "POST", "PUT", "DELETE"};
    return method >= 0 && method < 4 ? names[method] : "?";
}
static void new_uuid(char out[40]) {
    static uint32_t counter;
    uint64_t t = (uint64_t)time(NULL);
    uint32_t n = ++counter;
    snprintf(out, 40, "%08x-%04x-4%03x-8%03x-%012llx", (unsigned)(t & 0xffffffff), (unsigned)(n & 0xffff),
             (unsigned)(t >> 32 & 0xfff), (unsigned)(n * 2654435761u & 0xfff), (unsigned long long)(t * 1000003 + n) & 0xffffffffffffULL);
}
static Request *request_of(int64_t id) {
    for (int i = 0; i < MAX_REQUESTS; ++i) if (requests[i].used && requests[i].id == id) return &requests[i];
    return NULL;
}
static Session *session_of(const char *id) {
    for (int i = 0; i < MAX_SESSIONS; ++i) if (sessions[i].used && !strcmp(sessions[i].id, id)) return &sessions[i];
    return NULL;
}
static void set_body(Request *r, int status, const char *text) {
    free(r->body);
    r->status = status; r->body_size = text ? strlen(text) : 0; r->body_read = 0;
    r->body = r->body_size ? strdup(text) : NULL;
}
/* The sessionInvitation service: /v1/sessions and what hangs below it. */
static void answer_sessions(Request *r, const void *data, size_t size) {
    const char *p = r->path;
    const OnlineIdentity *me = runtime_online_identity();
    char text[1024];
    if (!strncmp(p, "/v1/sessions", 12) && (p[12] == 0 || p[12] == '?') && r->method == 1) { /* POST: a new session */
        Session *s = NULL;
        for (int i = 0; i < MAX_SESSIONS; ++i) if (!sessions[i].used) { s = &sessions[i]; break; }
        if (!s) { s = &sessions[0]; }
        memset(s, 0, sizeof(*s)); s->used = 1; new_uuid(s->id);
        if (data && size) { s->data_size = size < sizeof(s->data) ? size : sizeof(s->data); memcpy(s->data, data, s->data_size); }
        snprintf(text, sizeof(text), "{\"sessionId\":\"%s\"}", s->id);
        set_body(r, 200, text);
        return;
    }
    if (!strncmp(p, "/v1/sessions/", 13)) {
        char id[40]; size_t n = strcspn(p + 13, "/?");
        if (n >= sizeof(id)) n = sizeof(id) - 1;
        memcpy(id, p + 13, n); id[n] = 0;
        const char *rest = p + 13 + n;
        Session *s = session_of(id);
        if (!s && r->method != 3) {
            /* another player's session (every game answers its own web API): known from now on */
            for (int i = 0; i < MAX_SESSIONS; ++i) if (!sessions[i].used) { s = &sessions[i]; break; }
            if (!s) s = &sessions[0];
            memset(s, 0, sizeof(*s)); s->used = 1; snprintf(s->id, sizeof(s->id), "%s", id);
        }
        if (r->method == 3) { if (s) s->used = 0; set_body(r, 204, NULL); return; }              /* DELETE */
        if (r->method == 2 || r->method == 1) { set_body(r, 204, NULL); return; }                 /* PUT / POST: changes, members */
        if (!*rest || *rest == '?') {                                                              /* GET: the session */
            snprintf(text, sizeof(text),
                     "{\"sessionId\":\"%s\",\"sessionName\":\"WipEout\",\"sessionStatus\":\"\",\"sessionMaxUser\":8,"
                     "\"sessionType\":\"owner-migration\",\"sessionPrivacy\":\"public\",\"sessionLockFlag\":false,"
                     "\"sessionCreator\":{\"onlineId\":\"%s\",\"accountId\":\"%llu\",\"platform\":\"PS4\"},"
                     "\"members\":[{\"onlineId\":\"%s\",\"accountId\":\"%llu\",\"platform\":\"PS4\"}]}",
                     s->id, me->online_id, (unsigned long long)me->account_id, me->online_id, (unsigned long long)me->account_id);
            set_body(r, 200, text);
            return;
        }
        if (!strncmp(rest, "/sessionData", 12) || !strncmp(rest, "/changeableSessionData", 22)) {
            if (s->data_size) { free(r->body); r->status = 200; r->body = malloc(s->data_size); r->body_size = s->data_size; r->body_read = 0; if (r->body) memcpy(r->body, s->data, s->data_size); }
            else set_body(r, 200, "");
            return;
        }
        set_body(r, 200, "{}");
        return;
    }
    if (!strncmp(p, "/v1/users/", 10)) { /* my sessions, my invitations: none */
        set_body(r, 200, strstr(p, "invitation") ? "{\"size\":0,\"invitations\":[]}" : "{\"size\":0,\"sessions\":[]}");
        return;
    }
    set_body(r, 404, "{\"error\":{\"code\":2159617,\"message\":\"Not found\"}}");
}
static int equal_nocase(const char *a, const char *b) {
    for (; *a && *b; ++a, ++b) if ((*a | 32) != (*b | 32)) return 0;
    return *a == *b;
}
/* The group comes as "sessionInvitation" or "sdk:sessionInvitation". */
static void answer(Request *r, const void *data, size_t size) {
    if (strstr(r->group, "sessionInvitation")) answer_sessions(r, data, size);
    else if (strstr(r->group, "userProfile") && strstr(r->path, "/blockingUsers"))
        set_body(r, 200, "{\"blockingUsers\":[],\"size\":0,\"start\":0,\"totalResults\":0}");
    else if (strstr(r->group, "userProfile")) {
        const OnlineIdentity *me = runtime_online_identity();
        char text[512];
        snprintf(text, sizeof(text), "{\"profile\":{\"onlineId\":\"%s\",\"accountId\":\"%llu\",\"npId\":\"%s\",\"avatarUrl\":\"\",\"plus\":1,\"aboutMe\":\"\",\"languagesUsed\":[\"en\"],\"trophySummary\":{\"level\":1,\"progress\":0}}}",
                 me->online_id, (unsigned long long)me->account_id, me->online_id);
        set_body(r, 200, text);
    } else set_body(r, 404, "{\"error\":{\"code\":2159617,\"message\":\"Not supported\"}}");
    if (r->status >= 400) snprintf(r->error_object, sizeof(r->error_object), "%s", r->body ? r->body : "");
}

/* ---- the API ---- */
static ABI int32_t webapi_initialize(int32_t http_ctx, uint64_t pool_size) { (void)http_ctx; (void)pool_size; return new_id(); }
static ABI int32_t webapi_terminate(int32_t lib_ctx) { (void)lib_ctx; return 0; }
static ABI int32_t webapi_create_context(int32_t lib_ctx, int32_t user) {
    (void)lib_ctx;
    if (!runtime_online_identity()->online) return WEBAPI_NOT_SIGNED_IN;
    int id = new_id();
    TRACE("sceNpWebApiCreateContextA(user %d) -> %d", user, id);
    return id;
}
static ABI int32_t webapi_create_handle(int32_t lib_ctx) { (void)lib_ctx; return new_id(); }
static ABI int32_t webapi_zero(void) { return 0; }
/* SceNpWebApiContentParameter {size_t contentLength; const char *pContentType}. */
static ABI int32_t webapi_create_request(int32_t ctx, const char *group, const char *path, int32_t method, const uint64_t *content, int64_t *request_id) {
    if (!group || !path || !request_id) return WEBAPI_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    Request *r = NULL;
    for (int i = 0; i < MAX_REQUESTS; ++i) if (!requests[i].used) { r = &requests[i]; break; }
    if (!r) { pthread_mutex_unlock(&lock); return WEBAPI_INVALID_ARGUMENT; }
    memset(r, 0, sizeof(*r));
    r->used = 1; r->id = next_request++; r->ctx = ctx; r->method = method;
    snprintf(r->group, sizeof(r->group), "%s", group); snprintf(r->path, sizeof(r->path), "%s", path);
    *request_id = r->id;
    pthread_mutex_unlock(&lock);
    TRACE("sceNpWebApiCreateRequest(%s %s %s, %llu bytes) -> %lld", method_name(method), group, path,
          content ? (unsigned long long)content[0] : 0ull, (long long)r->id);
    return 0;
}
/* SceNpWebApiResponseInformationOption {int32 httpStatus; char *pErrorObject; size_t errorObjectSize; int64 responseDataLength}. */
static ABI int32_t webapi_send(int64_t request_id, const void *data, uint64_t size, uint8_t *info) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    if (!r) { pthread_mutex_unlock(&lock); return WEBAPI_NO_SUCH_REQUEST; }
    answer(r, data, (size_t)size);
    int32_t status = r->status; int64_t length = (int64_t)r->body_size;
    if (info) {
        memcpy(info, &status, 4);
        char *error_object; uint64_t error_size;
        memcpy(&error_object, info + 8, 8); memcpy(&error_size, info + 16, 8);
        if (status >= 400 && error_object && error_size) snprintf(error_object, (size_t)error_size, "%s", r->error_object);
        memcpy(info + 24, &length, 8);
    }
    pthread_mutex_unlock(&lock);
    TRACE("sceNpWebApiSendRequest2(%lld: %s %s) -> %d, %lld bytes%s%.200s", (long long)request_id, method_name(r->method), r->path,
          status, (long long)length, length ? ": " : "", r->body ? r->body : "");
    if (runtime_online_trace() && data && size && size < 4096) {
        char text[4096]; memcpy(text, data, (size_t)size); text[size] = 0;
        for (size_t i = 0; i < size; ++i) if ((unsigned char)text[i] < 32 && text[i] != '\n') text[i] = '.';
        TRACE("request body: %s", text);
    }
    return status >= 400 ? (int32_t)0x80552E20 /* SCE_NP_WEBAPI_ERROR_HTTP_RESPONSE */ : 0;
}
static ABI int32_t webapi_read(int64_t request_id, void *out, uint64_t size) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    if (!r || !out) { pthread_mutex_unlock(&lock); return WEBAPI_NO_SUCH_REQUEST; }
    size_t left = r->body_size - r->body_read, n = left < size ? left : (size_t)size;
    if (n) { memcpy(out, r->body + r->body_read, n); r->body_read += n; }
    pthread_mutex_unlock(&lock);
    return (int32_t)n;
}
static ABI int32_t webapi_status(int64_t request_id, int32_t *status) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    if (!r || !status) { pthread_mutex_unlock(&lock); return WEBAPI_NO_SUCH_REQUEST; }
    *status = r->status;
    pthread_mutex_unlock(&lock);
    return 0;
}
static const char *header_value(const Request *r, const char *name, char *buf, size_t size) {
    if (equal_nocase(name, "Content-Type")) return r->body_size ? "application/json; charset=utf-8" : NULL;
    if (equal_nocase(name, "Date")) {
        time_t now = time(NULL); struct tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &now);
#else
        gmtime_r(&now, &tm);
#endif
        strftime(buf, size, "%a, %d %b %Y %H:%M:%S GMT", &tm); return buf;
    }
    if (equal_nocase(name, "X-Psn-Request-Id")) return "0000000000000000";
    return NULL;
}
static ABI int32_t webapi_header_length(int64_t request_id, const char *name, uint64_t *length) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    char buf[64]; const char *value = r && name ? header_value(r, name, buf, sizeof(buf)) : NULL;
    if (length) *length = value ? strlen(value) : 0;
    pthread_mutex_unlock(&lock);
    return r ? 0 : WEBAPI_NO_SUCH_REQUEST;
}
static ABI int32_t webapi_header(int64_t request_id, const char *name, char *out, uint64_t size) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    char buf[64]; const char *value = r && name ? header_value(r, name, buf, sizeof(buf)) : NULL;
    if (out && size) snprintf(out, (size_t)size, "%s", value ? value : "");
    pthread_mutex_unlock(&lock);
    return r ? (value ? 0 : WEBAPI_INVALID_ARGUMENT) : WEBAPI_NO_SUCH_REQUEST;
}
static ABI int32_t webapi_delete_request(int64_t request_id) {
    pthread_mutex_lock(&lock);
    Request *r = request_of(request_id);
    if (r) { free(r->body); memset(r, 0, sizeof(*r)); }
    pthread_mutex_unlock(&lock);
    return r ? 0 : WEBAPI_NO_SUCH_REQUEST;
}
static ABI int32_t webapi_push_filter(void) { return new_id(); }
static ABI int32_t webapi_push_register(void) { return new_id(); }
static ABI int32_t webapi_pool_stats(int32_t lib_ctx, uint64_t *stats) { (void)lib_ctx; if (!stats) return WEBAPI_INVALID_ARGUMENT; stats[0] = 65536; stats[1] = 0; stats[2] = 0; return 0; }
/* sceNpWebApiUtilityParseNpId(const char *json, SceNpId *out): the online id string of a member record. */
static ABI int32_t webapi_parse_npid(const char *text, uint8_t *np_id) {
    if (!text || !np_id) return WEBAPI_INVALID_ARGUMENT;
    memset(np_id, 0, 36);
    const char *p = strstr(text, "\"onlineId\"");
    if (p) p = strchr(p + 10, '"');
    if (!p) return WEBAPI_INVALID_ARGUMENT;
    size_t n = strcspn(p + 1, "\"");
    memcpy(np_id, p + 1, n < 16 ? n : 16);
    return 0;
}

static const RuntimeExport exports[] = {
    {"sceNpWebApiInitialize", webapi_initialize}, {"sceNpWebApiTerminate", webapi_terminate},
    {"sceNpWebApiCreateContext", webapi_create_context}, {"sceNpWebApiCreateContextA", webapi_create_context},
    {"sceNpWebApiDeleteContext", webapi_zero}, {"sceNpWebApiCreateHandle", webapi_create_handle}, {"sceNpWebApiDeleteHandle", webapi_zero},
    {"sceNpWebApiCreateRequest", webapi_create_request}, {"sceNpWebApiSendRequest", webapi_send}, {"sceNpWebApiSendRequest2", webapi_send},
    {"sceNpWebApiReadData", webapi_read}, {"sceNpWebApiGetHttpStatusCode", webapi_status},
    {"sceNpWebApiGetHttpResponseHeaderValueLength", webapi_header_length}, {"sceNpWebApiGetHttpResponseHeaderValue", webapi_header},
    {"sceNpWebApiDeleteRequest", webapi_delete_request}, {"sceNpWebApiAbortRequest", webapi_zero},
    {"sceNpWebApiCreatePushEventFilter", webapi_push_filter}, {"sceNpWebApiDeletePushEventFilter", webapi_zero},
    {"sceNpWebApiRegisterPushEventCallback", webapi_push_register}, {"sceNpWebApiUnregisterPushEventCallback", webapi_zero},
    {"sceNpWebApiCreateExtdPushEventFilter", webapi_push_filter}, {"sceNpWebApiDeleteExtdPushEventFilter", webapi_zero},
    {"sceNpWebApiRegisterExtdPushEventCallbackA", webapi_push_register}, {"sceNpWebApiUnregisterExtdPushEventCallback", webapi_zero},
    {"sceNpWebApiGetMemoryPoolStats", webapi_pool_stats}, {"sceNpWebApiUtilityParseNpId", webapi_parse_npid},
};
uintptr_t runtime_webapi_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
