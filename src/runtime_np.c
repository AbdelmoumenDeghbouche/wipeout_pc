/* PSN account and network status for online play (libSceNpManager, libSceNetCtl).
 *
 * Offline (no WIPEOUT_NET_SERVER / WIPEOUT_NET_HOST): exactly the console with the cable unplugged
 * and nobody signed in, as before. Online: one local account signed in to a private "PSN":
 *   - online ID = the player name (WIPEOUT_USER_NAME, launcher: Game > Player name), 3..16 chars;
 *   - account ID = a hash of the online ID, stable across runs and machines;
 *   - the console's address is its address on the virtual LAN of the match server (10.77/16,
 *     runtime_wire.h), so anything the game tells other players about itself stays reachable;
 *   - PlayStation Plus, availability and reachability checks pass; async requests finish at once.
 * WIPEOUT_NET_TRACE=1 prints every call here and in runtime_net.c / runtime_matching.c. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_wire.h"
#include "runtime_online.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <ctype.h>
#include <pthread.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <unistd.h>
#endif

#define USER_ID 1
#define NP_OK 0
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_USER_NOT_FOUND ((int32_t)0x80550007)
#define NP_INVALID_ID ((int32_t)0x8055000C)
#define NP_STATE_SIGNED_OUT 1
#define NP_STATE_SIGNED_IN 2
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_STATE_DISCONNECTED 0
#define NET_CTL_STATE_IP_OBTAINED 3

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static int trace_enabled = -1;
static int next_id = 1;
static int new_id(void) { pthread_mutex_lock(&lock); int id = next_id++; pthread_mutex_unlock(&lock); return id; }

int runtime_online_trace(void) {
    if (trace_enabled < 0) { const char *v = getenv("WIPEOUT_NET_TRACE"); trace_enabled = v && *v && *v != '0'; }
    return trace_enabled;
}
static double trace_seconds(void) {
    static struct timespec start; static int started;
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    if (!started) { start = t; started = 1; }
    return (double)(t.tv_sec - start.tv_sec) + (double)(t.tv_nsec - start.tv_nsec) / 1e9;
}
void runtime_online_log(const char *fmt, ...) {
    if (!runtime_online_trace()) return;
    char line[1024];
    va_list args; va_start(args, fmt); vsnprintf(line, sizeof(line), fmt, args); va_end(args);
    printf("Net: [%.3f] %s\n", trace_seconds(), line);
    fflush(stdout);
}
void runtime_online_dump(const char *label, const void *data, size_t size) {
    if (!runtime_online_trace()) return;
    printf("Net: [%.3f] %s: %zu bytes\n", trace_seconds(), label, size);
    for (size_t off = 0; off < size; off += 32) {
        char line[160]; size_t pos = (size_t)snprintf(line, sizeof(line), "  %04zx:", off);
        for (size_t i = off; i < size && i < off + 32; ++i) pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %02x", ((const uint8_t *)data)[i]);
        printf("Net: %s\n", line);
    }
    fflush(stdout);
}
void runtime_online_hex(const char *label, const void *data, size_t size, size_t max) {
    if (!runtime_online_trace()) return;
    char line[1024];
    size_t n = size < max ? size : max, pos = 0;
    pos += (size_t)snprintf(line, sizeof(line), "%s: %zu bytes:", label, size);
    for (size_t i = 0; i < n && pos + 4 < sizeof(line); ++i)
        pos += (size_t)snprintf(line + pos, sizeof(line) - pos, " %02x", ((const uint8_t *)data)[i]);
    if (n < size && pos + 4 < sizeof(line)) snprintf(line + pos, sizeof(line) - pos, " ..");
    printf("Net: %s\n", line);
    fflush(stdout);
}
#define TRACE(...) runtime_online_log(__VA_ARGS__)

/* ---- identity ---- */
static OnlineIdentity identity;
static int identity_ready;
static uint64_t fnv1a(const char *text) {
    uint64_t h = UINT64_C(0xcbf29ce484222325);
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) { h ^= *p; h *= UINT64_C(0x100000001b3); }
    return h;
}
static void sanitize_online_id(const char *name, char out[WIRE_ONLINE_ID + 1]) {
    size_t n = 0;
    for (const char *p = name; *p && n < WIRE_ONLINE_ID; ++p) {
        unsigned char c = (unsigned char)*p;
        if (isalnum(c) || c == '-' || c == '_') out[n++] = (char)c;
        else if (c == ' ' && n) out[n++] = '_';
    }
    for (; n < 3; ++n) out[n] = n == 0 ? 'P' : (char)('0' + n); /* "P12": a PSN id has 3 characters at least */
    out[n] = 0;
}
/* The console's Ethernet address. RakNet derives its peer GUID from it, and a peer with the local
 * GUID counts as "self", so every player needs their own: locally administered, from the name. */
void runtime_online_mac(uint8_t mac[6]) {
    const uint64_t account = runtime_online_identity()->account_id;
    mac[0] = 0x02; mac[1] = 0x00;
    for (int i = 0; i < 4; ++i) mac[2 + i] = (uint8_t)(account >> (8 * i));
}
const OnlineIdentity *runtime_online_identity(void) {
    pthread_mutex_lock(&lock);
    if (!identity_ready) {
        const char *server = getenv("WIPEOUT_NET_SERVER"), *host = getenv("WIPEOUT_NET_HOST");
        identity.online = (server && *server) || (host && *host && *host != '0');
        const char *name = getenv("WIPEOUT_USER_NAME");
        if (name && *name) sanitize_online_id(name, identity.online_id);
        else { /* no name set: two PCs must still differ, the name is the player's identity */
            char machine[256] = "";
#ifdef _WIN32
            const char *computer = getenv("COMPUTERNAME");
            if (computer) snprintf(machine, sizeof(machine), "%s", computer);
#else
            gethostname(machine, sizeof(machine) - 1);
#endif
            char fallback[32];
            snprintf(fallback, sizeof(fallback), "Player-%04x", (unsigned)(fnv1a(machine) & 0xFFFF));
            sanitize_online_id(fallback, identity.online_id);
        }
        identity.account_id = fnv1a(identity.online_id) | (UINT64_C(1) << 62); /* never 0 */
        uint32_t h = (uint32_t)(identity.account_id ^ identity.account_id >> 32);
        h = (h ^ h >> 16) & 0xFFFF;
        if (h == 0 || h == 0xFFFF) h = 1;
        identity.client_id = (uint16_t)h;
        identity.virtual_ip = wire_virtual_ip(identity.client_id);
        identity_ready = 1;
        if (identity.online)
            printf("Net: online as %s (account %016llx, address 10.77.%u.%u)\n", identity.online_id,
                   (unsigned long long)identity.account_id, identity.client_id >> 8, identity.client_id & 255);
    }
    pthread_mutex_unlock(&lock);
    return &identity;
}
void runtime_online_set_client_id(uint16_t client_id) {
    pthread_mutex_lock(&lock);
    if (identity_ready && client_id && client_id != identity.client_id) {
        printf("Net: the server assigned address 10.77.%u.%u (wanted 10.77.%u.%u)\n", client_id >> 8, client_id & 255,
               identity.client_id >> 8, identity.client_id & 255);
        identity.client_id = client_id; identity.virtual_ip = wire_virtual_ip(client_id);
    }
    pthread_mutex_unlock(&lock);
}
static int online(void) { return runtime_online_identity()->online; }

/* SceNpId: SceNpOnlineId {data[16], term, dummy[3]}, opt[8], reserved[8] = 36 bytes. */
typedef struct { char data[16]; char term; char dummy[3]; } NpOnlineId;
typedef struct { NpOnlineId handle; uint8_t opt[8]; uint8_t reserved[8]; } NpId;
static void fill_online_id(NpOnlineId *id) {
    memset(id, 0, sizeof(*id));
    memcpy(id->data, runtime_online_identity()->online_id, strnlen(runtime_online_identity()->online_id, 16));
}
void runtime_online_np_id(void *np_id_36) {
    NpId *id = np_id_36; memset(id, 0, sizeof(*id)); fill_online_id(&id->handle);
}

/* ---- NpManager ---- */
static ABI int32_t np_get_state(int32_t user, int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    *state = user == USER_ID && online() ? NP_STATE_SIGNED_IN : NP_STATE_SIGNED_OUT;
    TRACE("sceNpGetState(user %d) -> %s", user, *state == NP_STATE_SIGNED_IN ? "signed in" : "signed out");
    return 0;
}
static ABI int32_t np_get_np_id(int32_t user, NpId *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    memset(id, 0, sizeof(*id)); fill_online_id(&id->handle);
    TRACE("sceNpGetNpId -> %s", id->handle.data);
    return 0;
}
static ABI int32_t np_get_online_id(int32_t user, NpOnlineId *id) {
    if (!id) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    fill_online_id(id);
    TRACE("sceNpGetOnlineId -> %s", id->data);
    return 0;
}
static ABI int32_t np_get_account_id(int32_t user, uint64_t *account) {
    if (!account) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    *account = runtime_online_identity()->account_id;
    TRACE("sceNpGetAccountIdA -> %016llx", (unsigned long long)*account);
    return 0;
}
static ABI int32_t np_get_user_by_account(uint64_t account, int32_t *user) {
    if (!user) return NP_INVALID_ARGUMENT;
    if (!online() || account != runtime_online_identity()->account_id) return NP_USER_NOT_FOUND;
    *user = USER_ID; return 0;
}
static ABI int32_t np_get_country(int32_t user, char *country) {
    if (!country) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    memcpy(country, "us\0\0", 4); return 0;
}
static ABI int32_t np_get_language(int32_t user, char *language) {
    if (!language) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    memset(language, 0, 8); memcpy(language, "en", 2); return 0;
}
static ABI int32_t np_get_date_of_birth(int32_t user, uint8_t *date) {
    if (!date) return NP_INVALID_ARGUMENT;
    if (user != USER_ID) return NP_USER_NOT_FOUND;
    if (!online()) return NP_SIGNED_OUT;
    const uint16_t year = 1990; memcpy(date, &year, 2); date[2] = 1; date[3] = 1; return 0;
}
/* Async requests (sceNpCreateAsyncRequest / sceNpCreateRequest): the work completes inside the
 * call that uses the request, so polling finds the result immediately. */
static ABI int32_t np_create_request(const void *param) { (void)param; int id = new_id(); TRACE("sceNpCreateRequest -> %d", id); return id; }
static ABI int32_t np_delete_request(int32_t id) { (void)id; return 0; }
/* sceNpPollAsync: 0 = the request finished (its result in *result), 1 = still running. */
static ABI int32_t np_poll(int32_t request, int32_t *result) {
    if (result) *result = online() ? 0 : NP_SIGNED_OUT;
    TRACE("sceNpPollAsync(%d) -> finished, result %#x", request, result ? (unsigned)*result : 0u);
    return 0;
}
static ABI int32_t np_check_availability(int32_t request, int32_t user, void *reserved) {
    (void)request; (void)reserved;
    TRACE("sceNpCheckNpAvailabilityA(user %d) -> %s", user, online() ? "available" : "signed out");
    return user == USER_ID && online() ? 0 : NP_SIGNED_OUT;
}
static ABI int32_t np_check_reachability(int32_t request, int32_t user) {
    (void)request; (void)user;
    return online() ? 0 : NP_SIGNED_OUT;
}
static ABI int32_t np_reachability_state(int32_t user, int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user;
    *state = online() ? 2 : 0; /* REACHABLE / UNAVAILABLE */
    return 0;
}
/* SceNpCheckPlusParameter {size, userId @8, features @16}; SceNpCheckPlusResult {bool authorized}. */
static ABI int32_t np_check_plus(int32_t request, const uint8_t *param, uint8_t *result) {
    (void)request;
    if (!param || !result) return NP_INVALID_ARGUMENT;
    int32_t user; memcpy(&user, param + 8, 4);
    if (!online() || user != USER_ID) { result[0] = 0; return NP_SIGNED_OUT; }
    result[0] = 1; /* PlayStation Plus features are available */
    TRACE("sceNpCheckPlus(user %d) -> authorized", user);
    return 0;
}
static ABI int32_t np_notify_plus(const void *param) { (void)param; TRACE("sceNpNotifyPlusFeature"); return online() ? 0 : NP_SIGNED_OUT; }
static ABI int32_t np_content_restriction(const void *param) { (void)param; return 0; }
static ABI int32_t np_parental(int32_t user, int32_t *age, uint8_t *info) {
    (void)user;
    if (!online()) return NP_SIGNED_OUT;
    if (age) *age = 30;
    if (info) memset(info, 0, 4); /* no chat / UGC / content restrictions */
    return 0;
}
/* State callbacks: the state never changes while the game runs, so none is delivered. */
typedef ABI void (*NpStateCallback)(int32_t user, int32_t state, void *arg);
static NpStateCallback state_callback; static void *state_callback_arg; static int state_callback_pending;
static ABI int32_t np_register_state(NpStateCallback cb, void *arg) {
    pthread_mutex_lock(&lock); state_callback = cb; state_callback_arg = arg; state_callback_pending = cb != NULL; pthread_mutex_unlock(&lock);
    TRACE("sceNpRegisterStateCallback(%p)", (void *)cb);
    return new_id();
}
static ABI int32_t np_unregister_state(int32_t id) { (void)id; pthread_mutex_lock(&lock); state_callback = NULL; pthread_mutex_unlock(&lock); return 0; }
static ABI int32_t np_register_state_toolkit(NpStateCallback cb, void *arg) { np_register_state(cb, arg); return 0; }
static ABI int32_t np_unregister_state_toolkit(void) { return np_unregister_state(0); }
/* sceNpCheckCallback / ForLib: the one place state callbacks run (on the game's thread). The
 * first check after registration reports the current state once, as the console does at start. */
static ABI int32_t np_check_callback(void) {
    pthread_mutex_lock(&lock);
    NpStateCallback cb = state_callback; void *arg = state_callback_arg; int pending = state_callback_pending; state_callback_pending = 0;
    pthread_mutex_unlock(&lock);
    if (cb && pending) {
        TRACE("Np state callback: user %d %s", USER_ID, online() ? "signed in" : "signed out");
        cb(USER_ID, online() ? NP_STATE_SIGNED_IN : NP_STATE_SIGNED_OUT, arg);
    }
    return 0;
}
static ABI int32_t np_ok(void) { return 0; }
static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI void np_register_void(void *cb, void *arg) { (void)cb; (void)arg; }

/* ---- NetCtl ---- */
static ABI int32_t netctl_state(int32_t *state) {
    if (!state) return NET_CTL_INVALID_ADDR;
    *state = online() ? NET_CTL_STATE_IP_OBTAINED : NET_CTL_STATE_DISCONNECTED;
    return 0;
}
static void ip_string(uint32_t ip, char *out, size_t size) {
    snprintf(out, size, "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 255, (ip >> 8) & 255, ip & 255);
}
/* SceNetCtlInfo: a union; strings are 16 bytes, numbers 32-bit. */
static ABI int32_t netctl_info(int32_t code, uint8_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    if (!online()) return NET_CTL_NOT_CONNECTED;
    const OnlineIdentity *id = runtime_online_identity();
    uint32_t value = 0; char text[64] = "";
    switch (code) {
    case 1: value = 0; break;                           /* device: wired */
    case 2: runtime_online_mac(info); return 0;         /* ether address */
    case 3: value = 1500; break;                        /* mtu */
    case 4: value = 1; break;                           /* link: connected */
    case 11: value = 0; break;                          /* ip config: dhcp */
    case 12: break;                                     /* dhcp hostname */
    case 14: ip_string(id->virtual_ip, text, sizeof(text)); break;
    case 15: snprintf(text, sizeof(text), "255.255.0.0"); break;
    case 16: ip_string(WIRE_VIRTUAL_NET | 1, text, sizeof(text)); break; /* default route */
    case 17: case 18: snprintf(text, sizeof(text), "10.77.0.1"); break;  /* dns */
    case 19: value = 0; break;                          /* http proxy: off */
    case 20: break;
    case 21: value = 0; break;
    default: TRACE("sceNetCtlGetInfo(%d): unknown code", code); return NET_CTL_INVALID_ADDR;
    }
    if (code == 12 || code == 14 || code == 15 || code == 16 || code == 17 || code == 18 || code == 20) {
        memset(info, 0, 16); memcpy(info, text, strnlen(text, 15));
    } else memcpy(info, &value, 4);
    TRACE("sceNetCtlGetInfo(%d) -> %s%u", code, text, text[0] ? 0 : value);
    return 0;
}
/* SceNetCtlNatInfo {int size; int stunStatus; int natType; SceNetInAddr mappedAddr}. */
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    if (!online()) { info[1] = 0; info[2] = 3; info[3] = 0; return 0; }
    info[1] = 1; info[2] = 2; info[3] = htonl(runtime_online_identity()->virtual_ip); /* stun ok, NAT type 2 */
    TRACE("sceNetCtlGetNatInfo -> type 2");
    return 0;
}
static ABI int32_t netctl_register(void *cb, void *arg, int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid = new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int32_t cid) { (void)cid; return 0; }
static ABI int32_t netctl_result(int32_t event, int32_t *error) { (void)event; if (error) *error = 0; return 0; }

static const RuntimeExport exports[] = {
    {"sceNpGetState", np_get_state}, {"sceNpGetNpId", np_get_np_id}, {"sceNpGetOnlineId", np_get_online_id},
    {"sceNpGetAccountIdA", np_get_account_id}, {"sceNpGetUserIdByAccountId", np_get_user_by_account},
    {"sceNpGetAccountCountryA", np_get_country}, {"sceNpGetAccountLanguageA", np_get_language},
    {"sceNpGetAccountDateOfBirthA", np_get_date_of_birth},
    {"sceNpCreateAsyncRequest", np_create_request}, {"sceNpCreateRequest", np_create_request},
    {"sceNpDeleteRequest", np_delete_request}, {"sceNpAbortRequest", np_delete_request}, {"sceNpPollAsync", np_poll},
    {"sceNpCheckNpAvailability", np_signed_out}, {"sceNpCheckNpAvailabilityA", np_check_availability},
    {"sceNpCheckNpReachability", np_check_reachability}, {"sceNpGetNpReachabilityState", np_reachability_state},
    {"sceNpCheckPlus", np_check_plus}, {"sceNpNotifyPlusFeature", np_notify_plus},
    {"sceNpSetContentRestriction", np_content_restriction}, {"sceNpGetParentalControlInfo", np_parental},
    {"sceNpRegisterStateCallback", np_register_state}, {"sceNpUnregisterStateCallback", np_unregister_state},
    {"sceNpRegisterStateCallbackForToolkit", np_register_state_toolkit},
    {"sceNpUnregisterStateCallbackForToolkit", np_unregister_state_toolkit},
    {"sceNpCheckCallback", np_check_callback}, {"sceNpCheckCallbackForLib", np_check_callback},
    {"sceNpRegisterGamePresenceCallback", np_register_void}, {"sceNpGetGamePresenceStatus", np_signed_out},
    {"sceNpRegisterPlusEventCallback", np_create_request}, {"sceNpUnregisterPlusEventCallback", np_ok},
    {"sceNpSetNpTitleId", np_ok},
    {"sceNetCtlGetState", netctl_state}, {"sceNetCtlGetInfo", netctl_info}, {"sceNetCtlGetNatInfo", netctl_nat},
    {"sceNetCtlRegisterCallback", netctl_register}, {"sceNetCtlCheckCallback", netctl_check},
    {"sceNetCtlUnregisterCallback", netctl_unregister}, {"sceNetCtlGetResult", netctl_result},
    {"sceNetCtlRegisterCallbackForNpToolkit", netctl_register}, {"sceNetCtlUnregisterCallbackForNpToolkit", netctl_unregister},
    {"sceNetCtlCheckCallbackForNpToolkit", netctl_check}, {"sceNetCtlInit", np_ok}, {"sceNetCtlTerm", np_ok},
};
uintptr_t runtime_np_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }
