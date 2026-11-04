/* System services for an offline, single-user console:
 *   - one local user (id 1) who is logged in but not signed in to PSN;
 *   - network cable unplugged: NetCtl disconnected, sockets unavailable;
 *   - common dialogs complete immediately (no UI is drawn yet);
 *   - the whole package is installed (PlayGo reports every chunk local).
 * Every entry here is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include "runtime_online.h"
#include "gpu/wipeoutgpu.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <time.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#endif

#define USER_ID 1
#define USER_NAME "Player"
#define USER2_ID 2 /* split-screen player 2 (#80): logged in while runtime_pad_second_user() */
#define USER2_NAME "Player 2"
#define ORBIS_OK 0
#define USER_INVALID_ARGUMENT ((int32_t)0x80960005)
#define USER_NO_EVENT ((int32_t)0x80960007)
#define SYSTEM_NO_EVENT ((int32_t)0x80A10004)
#define SYSTEM_PARAMETER ((int32_t)0x80A10003)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_CTL_INVALID_ADDR ((int32_t)0x80412107)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NP_INVALID_ARGUMENT ((int32_t)0x80550003)
#define NET_ENETUNREACH ((int32_t)0x80410133)
#define NET_EINVAL ((int32_t)0x80410116)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define DIALOG_NOT_INITIALIZED ((int32_t)0x80B80003)
#define PLAYGO_BAD_HANDLE ((int32_t)0x80B20009)
#define PLAYGO_BAD_POINTER ((int32_t)0x80B2000A)
#define PLAYGO_BAD_SIZE ((int32_t)0x80B2000B)
#define PLAYGO_BAD_CHUNK ((int32_t)0x80B2000C)
#define AUDIO_IN_NOT_OPENED ((int32_t)0x80260109)
#define TROPHY_INVALID ((int32_t)0x80551604)

static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static int next_id=1;
static int new_id(void) { pthread_mutex_lock(&lock); int id=next_id++; pthread_mutex_unlock(&lock); return id; }
static void note(const char *what) { printf("Runtime: %s\n",what); }

/* ---- UserService ---- */
static int login_event_pending=1, user2_reported;
static ABI int32_t user_initialize(const void *params) { (void)params; note("UserService initialized (user 1 logged in)"); return 0; }
static ABI int32_t user_terminate(void) { return 0; }
static ABI int32_t user_initial(int32_t *id) { if (!id) return USER_INVALID_ARGUMENT; *id=USER_ID; return 0; }
static ABI int32_t user_list(int32_t *ids) {
    if (!ids) return USER_INVALID_ARGUMENT;
    const int present=runtime_pad_second_user();
    pthread_mutex_lock(&lock);
    user2_reported=present; /* the game knows it now */
    pthread_mutex_unlock(&lock);
    ids[0]=USER_ID; ids[1]=present ? USER2_ID : -1; ids[2]=ids[3]=-1; return 0;
}
/* 1 when user 2 logged in or out since the game last heard; *present is the new state. */
static int user2_changed(int *present) {
    *present=runtime_pad_second_user();
    pthread_mutex_lock(&lock);
    const int changed=*present!=user2_reported; user2_reported=*present;
    pthread_mutex_unlock(&lock);
    if (changed) printf("Runtime: user 2 logged %s\n",*present ? "in" : "out");
    return changed;
}
/* The game learns about logins from a callback (sceUserServiceRegisterEventCallback). The pad
 * module calls runtime_user_events from player 1's pad read, about twice a second, and the
 * callback runs there, on that game thread. */
typedef ABI void (*UserEventCallback)(const int32_t *event,void *arg);
static UserEventCallback user_callback;
static void *user_callback_arg;
static ABI int32_t user_register_callback(UserEventCallback callback,void *arg) {
    pthread_mutex_lock(&lock); user_callback=callback; user_callback_arg=arg; pthread_mutex_unlock(&lock);
    return 0;
}
static ABI int32_t user_unregister_callback(void) {
    pthread_mutex_lock(&lock); user_callback=NULL; pthread_mutex_unlock(&lock);
    return 0;
}
void runtime_user_events(void) {
    static unsigned calls;
    pthread_mutex_lock(&lock);
    UserEventCallback callback=user_callback; void *arg=user_callback_arg;
    pthread_mutex_unlock(&lock);
    if (!callback || ++calls%30) return;
    int present;
    if (!user2_changed(&present)) return;
    const int32_t event[2]={present ? 0 : 1,USER2_ID}; /* LOGIN / LOGOUT */
    callback(event,arg);
}
/* The first player's name: WIPEOUT_USER_NAME (launcher: Game > Player name, #129), else "Player". */
static const char *player_name(void) {
    const char *name=getenv("WIPEOUT_USER_NAME");
    return name && *name ? name : USER_NAME;
}
static ABI int32_t user_name(int32_t id,char *name,uint64_t size) {
    if ((id!=USER_ID && id!=USER2_ID) || !name) return USER_INVALID_ARGUMENT;
    const char *text=id==USER_ID ? player_name() : USER2_NAME;
    if (strlen(text)+1>size) return (int32_t)0x8096000a; /* BUFFER_TOO_SHORT */
    strcpy(name,text); return 0;
}
static ABI int32_t user_event(int32_t *event) {
    if (!event) return USER_INVALID_ARGUMENT;
    pthread_mutex_lock(&lock);
    int pending=login_event_pending; login_event_pending=0;
    pthread_mutex_unlock(&lock);
    if (pending) { event[0]=0; event[1]=USER_ID; return 0; } /* LOGIN */
    /* User 2 logs in and out with the second gamepad. */
    int present;
    if (!user2_changed(&present)) return USER_NO_EVENT;
    event[0]=present ? 0 : 1; event[1]=USER2_ID; /* LOGIN / LOGOUT */
    return 0;
}

/* ---- SystemService ---- */
static int language(void) { const char *v=getenv("WIPEOUT_LANGUAGE"); return v ? atoi(v) : 1; }
static ABI int32_t system_param(int32_t id,int32_t *value) {
    if (!value) return SYSTEM_PARAMETER;
    switch (id) {
    case 1: *value=language(); break;           /* language (1 = English US, 8 = Russian) */
    case 2: *value=1; break;                    /* date format DD/MM/YYYY */
    case 3: *value=1; break;                    /* 24-hour clock */
#ifdef _WIN32
    case 4: *value=(int32_t)(compat_utc_offset(time(NULL))/60); break;
#else
    case 4: { time_t now=time(NULL); struct tm t; localtime_r(&now,&t); *value=(int32_t)(t.tm_gmtoff/60); break; }
#endif
    case 5: *value=0; break;                    /* summer time */
    case 7: *value=0; break;                    /* parental level off */
    case 1000: *value=1; break;                 /* enter button = cross */
    default: fprintf(stderr,"STOP: unsupported system parameter %d\n",id); exit(21);
    }
    return 0;
}
static ABI int32_t system_status(unsigned char *status) {
    if (!status) return SYSTEM_PARAMETER;
    memset(status,0,12); /* event_num=0, no overlay, foreground, normal CPU mode */
    return 0;
}
static ABI int32_t system_event(void *event) { (void)event; return SYSTEM_NO_EVENT; }
static ABI int32_t hide_splash(void) { note("SystemService: splash screen hidden"); return 0; }
static ABI int32_t safe_area(float *info) { if (!info) return SYSTEM_PARAMETER; *info=1.0f; return 0; } /* full screen */
static ABI int32_t disable_music_player(void) { return 0; }
static ABI int32_t launch_browser(void) { note("SystemService: web browser request ignored (offline)"); return 0; }

/* ---- NetCtl / Net / Http / Ssl: no network ---- */
static int32_t net_errno;
static ABI int32_t net_init(void) { return 0; }
static ABI int32_t net_term(void) { return 0; }
static ABI int32_t *net_errno_loc(void) { return &net_errno; }
static ABI int32_t net_pool_create(const char *name,int size,int flags) { (void)name; (void)size; (void)flags; return new_id(); }
static ABI int32_t net_pool_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_unreachable(void) { net_errno=51; return NET_ENETUNREACH; }
static ABI int32_t net_epoll_create(const char *name,int flags) { (void)name; (void)flags; return new_id(); }
static ABI int32_t net_epoll_destroy(int id) { (void)id; return 0; }
static ABI int32_t net_resolver_create(const char *name,int pool,int flags) { (void)name; (void)pool; (void)flags; return new_id(); }
static ABI int32_t net_resolver_destroy(int id) { (void)id; return 0; }
static ABI uint16_t net_htons(uint16_t v) { return htons(v); }
static ABI uint16_t net_ntohs(uint16_t v) { return ntohs(v); }
static ABI uint32_t net_htonl(uint32_t v) { return htonl(v); }
static ABI uint32_t net_ntohl(uint32_t v) { return ntohl(v); }
static ABI int32_t net_pton(int af,const char *src,void *dst) {
    if (af!=2) { net_errno=47; return NET_EINVAL; }
    return inet_pton(AF_INET,src,dst);
}
static ABI const char *net_ntop(int af,const void *src,char *dst,uint32_t size) {
    if (af!=2) { net_errno=47; return NULL; }
    return inet_ntop(AF_INET,src,dst,size);
}
static ABI int32_t netctl_state(int32_t *state) { if (!state) return NET_CTL_INVALID_ADDR; *state=0; return 0; }
static ABI int32_t netctl_info(int code,void *info) { (void)code; (void)info; return NET_CTL_NOT_CONNECTED; }
static ABI int32_t netctl_register(void *cb,void *arg,int32_t *cid) { (void)cb; (void)arg; if (!cid) return NET_CTL_INVALID_ADDR; *cid=new_id(); return 0; }
static ABI int32_t netctl_check(void) { return 0; }
static ABI int32_t netctl_unregister(int cid) { (void)cid; return 0; }
static ABI int32_t netctl_nat(uint32_t *info) {
    if (!info) return NET_CTL_INVALID_ADDR;
    info[1]=0; info[2]=3; info[3]=0; /* stun failed, NAT type 3, no mapped address */
    return 0;
}
static ABI int32_t ok_void(void) { return 0; }
/* Objects are created so setup code proceeds; any transfer fails as unplugged. */
static ABI int32_t http_epoll(int32_t ctx,void **handle) {
    (void)ctx;
    if (!handle) return (int32_t)0x80431077; /* HTTP INVALID_VALUE */
    *handle=(void *)(uintptr_t)(0x100+new_id()); return 0;
}
static ABI int32_t http_wait(void *handle,void *events,int32_t max,int64_t timeout) {
    (void)handle; (void)events; (void)max;
#ifdef _WIN32
    if (timeout>0) compat_sleep_ns((uint64_t)timeout*1000);
#else
    if (timeout>0) { struct timespec t={timeout/1000000,(timeout%1000000)*1000}; nanosleep(&t,NULL); }
#endif
    return 0; /* no events: nothing is in flight */
}

/* ---- NP (PSN): signed out ---- */
static ABI int32_t np_state(int32_t user,int32_t *state) {
    if (!state) return NP_INVALID_ARGUMENT;
    (void)user; *state=1; /* SIGNED_OUT */
    return 0;
}
static ABI int32_t np_register(void *cb,void *arg) { (void)cb; (void)arg; return new_id(); }
static ABI void np_register_void(void *cb,void *arg) { (void)cb; (void)arg; }
static ABI int32_t np_poll(int32_t request,int32_t *result) { runtime_online_log("sceNp*PollAsync(%d) (stub -> signed out)",request); if (result) *result=NP_SIGNED_OUT; return 0; }
static ABI int32_t np_compare(const void *a,const void *b) {
    if (!a || !b) return NP_INVALID_ARGUMENT;
    return memcmp(a,b,16) ? (int32_t)0x80550609 : 0; /* NP_UTIL NOT_MATCH */
}

/* ---- Voice chat: ports exist, carry no audio ---- */
static ABI int32_t voice_port(void *param,uint32_t *port) { (void)param; if (!port) return (int32_t)0x8029000b; *port=(uint32_t)new_id(); return 0; }
static ABI int32_t voice_read(uint32_t port,void *data,uint32_t *size) { (void)port; (void)data; if (size) *size=0; return 0; }
static ABI int32_t voice_write(uint32_t port,const void *data,uint32_t *size) { (void)port; (void)data; (void)size; return 0; }
static ABI int32_t voice_info(uint32_t port,uint32_t *info) {
    (void)port;
    if (!info) return (int32_t)0x8029000b;
    memset(info,0,40); /* type, state=unconnected, no bytes available */
    return 0;
}

/* ---- Common dialogs: nothing is displayed; an opened dialog finishes. ---- */
typedef struct { const char *name; int initialized, status; uint32_t button; } Dialog;
static Dialog dialogs[]={{"CommonDialog",0,0,0},{"MsgDialog",0,0,0},{"SaveDataDialog",0,0,0},
                         {"NpProfileDialog",0,0,0},{"NpCommerceDialog",0,0,0},{"ImeDialog",0,0,0}};
#define DIALOG_RUNNING 2
#define DIALOG_FINISHED 3
#define DIALOG_NOT_FINISHED ((int32_t)0x80B80005)
#define DIALOG_PARAM_INVALID ((int32_t)0x80B8000A)
#define DIALOG_NOT_RUNNING ((int32_t)0x80B8000B)
#define DIALOG_ARG_NULL ((int32_t)0x80B8000D)
#define DIALOG_NOT_SUPPORTED ((int32_t)0x80B8000F)
static ABI int32_t common_init(void) { return 0; }
static int32_t dialog_init(int i) {
    if (dialogs[i].initialized) return (int32_t)0x80B80004;
    dialogs[i].initialized=1; dialogs[i].status=1; return 0;
}
static int32_t dialog_open(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s opened; completed immediately (no dialog UI yet)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_FINISHED; dialogs[i].button=1; return 0; /* OK / YES */
}
/* A progress bar shows the game's own work: it runs until the game closes it. */
static int32_t dialog_open_progress(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    printf("Runtime: %s progress bar opened (runs until closed)\n",dialogs[i].name);
    dialogs[i].status=DIALOG_RUNNING; dialogs[i].button=0; return 0;
}
static int32_t dialog_close(int i) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    dialogs[i].status=DIALOG_FINISHED; return 0;
}
static int32_t dialog_progress(int i, uint32_t target) {
    if (dialogs[i].status!=DIALOG_RUNNING) return DIALOG_NOT_RUNNING;
    return target ? DIALOG_PARAM_INVALID : 0;
}
/* Result: result (OK), button, reserved; SaveDataDialog's begins with the mode. */
static int32_t dialog_result(int i, unsigned char *out, size_t offset) {
    if (dialogs[i].status!=DIALOG_FINISHED) return DIALOG_NOT_FINISHED;
    if (!out) return DIALOG_ARG_NULL;
    uint32_t values[2]={0,dialogs[i].button};
    memcpy(out+offset,values,8);
    return 0;
}
static int32_t dialog_status(int i) { return dialogs[i].status; }
static int32_t dialog_term(int i) {
    if (!dialogs[i].initialized) return DIALOG_NOT_INITIALIZED;
    dialogs[i].initialized=0; dialogs[i].status=0; return 0;
}
#define DIALOG(tag,i) \
    static ABI int32_t tag##_init(void) { return dialog_init(i); } \
    static ABI int32_t tag##_open(const void *p) { (void)p; return dialog_open(i); } \
    static ABI int32_t tag##_status(void) { return dialog_status(i); } \
    static ABI int32_t tag##_term(void) { return dialog_term(i); }
DIALOG(profile,3) DIALOG(commerce,4)
/* OrbisMsgDialogParam: BaseParam (48 bytes), size, mode (2 = progress bar).
 * OrbisSaveDataDialogParam: BaseParam, int32 size, mode (5 = progress bar). */
static int32_t dialog_open_mode(int i, const unsigned char *param, size_t mode_offset, uint32_t progress_mode) {
    if (!param) return DIALOG_ARG_NULL;
    uint32_t mode; memcpy(&mode,param+mode_offset,4);
    return mode==progress_mode ? dialog_open_progress(i) : dialog_open(i);
}
static ABI int32_t msg_init(void) { return dialog_init(1); }
static ABI int32_t msg_open(const unsigned char *p) { return dialog_open_mode(1,p,56,2); }
static ABI int32_t msg_status(void) { return dialog_status(1); }
static ABI int32_t msg_term(void) { return dialog_term(1); }
static ABI int32_t msg_close(void) { return dialog_close(1); }
static ABI int32_t msg_progress(uint32_t target, uint32_t value) { (void)value; return dialog_progress(1,target); }
static ABI int32_t msg_result(unsigned char *out) { return dialog_result(1,out,0); }
/* SaveDataDialog. OrbisSaveDataDialogParam: BaseParam (48), size, mode @52, dispType @56,
 * animParam @64, items @72, ..., userData @112. SaveDialogItems: userId @0, titleId @8,
 * dirName @16 (32-byte names), dirNameNum @24, newItem @32 (title @0), focusPos @40.
 * A list (mode 1) is shown for the player to choose; every other mode answers for the
 * first listed save. The result names the chosen save ("" = the new item). */
#define SAVE_MODE_LIST 1
#define SAVE_RESULT_CANCELED 1
static struct {
    uint32_t mode, result;
    void *user_data;
    char dirs[32][32];
    int dir_count, has_new, choosing;
} save_dialog;
static char save_labels[33][200], save_icons[33][720];
static void save_label(char *out, size_t size, const char *dir) {
    unsigned char param[RUNTIME_SAVE_PARAM_SIZE];
    if (runtime_savedata_param(USER_ID,dir,param)) { snprintf(out,size,"%s",dir); return; }
    int64_t mtime; memcpy(&mtime,param+1288,8);
    time_t t=(time_t)mtime; struct tm tm;
#ifdef _WIN32
    localtime_s(&tm,&t);
#else
    localtime_r(&t,&tm);
#endif
    char when[32]; strftime(when,sizeof(when),"%Y-%m-%d %H:%M",&tm);
    const char *title=(const char *)param, *subtitle=(const char *)param+128;
    /* Title, subtitle and date on their own lines, beside the save's picture. */
    snprintf(out,size,"%.60s\n%.60s%s%s",title[0] ? title : dir,subtitle,subtitle[0] ? "\n" : "",when);
}
static ABI int32_t save_init(void) { return dialog_init(2); }
static ABI int32_t save_open(const unsigned char *p) {
    if (!p) return DIALOG_ARG_NULL;
    if (!dialogs[2].initialized) return DIALOG_NOT_INITIALIZED;
    memset(&save_dialog,0,sizeof(save_dialog));
    memcpy(&save_dialog.mode,p+52,4);
    memcpy(&save_dialog.user_data,p+112,8);
    const unsigned char *items; memcpy(&items,p+72,8);
    if (items) {
        const char *dirs; uint32_t count; const void *new_item;
        memcpy(&dirs,items+16,8); memcpy(&count,items+24,4); memcpy(&new_item,items+32,8);
        for (uint32_t i=0;dirs && i<count && save_dialog.dir_count<32;++i)
            if (dirs[i*32]) memcpy(save_dialog.dirs[save_dialog.dir_count++],dirs+i*32,31);
        save_dialog.has_new=new_item!=NULL;
    }
    if (save_dialog.mode!=SAVE_MODE_LIST) return dialog_open_mode(2,p,52,5);
    const char *labels[33], *icons[33]; int n=0;
    if (save_dialog.has_new) { snprintf(save_labels[n],sizeof(save_labels[n]),"New save"); labels[n]=save_labels[n]; icons[n]=NULL; ++n; }
    for (int i=0;i<save_dialog.dir_count;++i) {
        save_label(save_labels[n],sizeof(save_labels[n]),save_dialog.dirs[i]); labels[n]=save_labels[n];
        icons[n]=runtime_savedata_icon(USER_ID,save_dialog.dirs[i],save_icons[n],sizeof(save_icons[n])) ? NULL : save_icons[n];
        ++n;
    }
    uint32_t type; memcpy(&type,p+56,4);
    const char *title=type==1 ? "Save game" : type==2 ? "Load game" : "Delete save";
    if (!n || !wipeoutgpu_choice_begin(title,labels,icons,n,0)) {
        printf("Runtime: SaveDataDialog list (%s): %s\n",title,n ? "no window, cancelled" : "no saves, cancelled");
        save_dialog.result=SAVE_RESULT_CANCELED;
        dialogs[2].status=DIALOG_FINISHED; dialogs[2].button=0;
        return 0;
    }
    printf("Runtime: SaveDataDialog list (%s): %d entries shown\n",title,n);
    save_dialog.choosing=1;
    dialogs[2].status=DIALOG_RUNNING;
    return 0;
}
static ABI int32_t save_status(void) {
    if (save_dialog.choosing) {
        int choice=wipeoutgpu_choice_poll();
        if (choice!=-1) {
            save_dialog.choosing=0;
            dialogs[2].status=DIALOG_FINISHED;
            if (choice<0) { save_dialog.result=SAVE_RESULT_CANCELED; dialogs[2].button=0; }
            else {
                int dir=choice-save_dialog.has_new;
                /* The chosen save goes first; the new item has no name. */
                if (dir>=0) memcpy(save_dialog.dirs[0],save_dialog.dirs[dir],32);
                else save_dialog.dirs[0][0]=0;
                save_dialog.dir_count=dir>=0 || save_dialog.dir_count ? 1 : 0;
                dialogs[2].button=1;
            }
            printf("Runtime: SaveDataDialog list: %s%s\n",choice<0 ? "cancelled" : "chose ",
                   choice<0 ? "" : save_dialog.dirs[0][0] ? save_dialog.dirs[0] : "new save");
        }
    }
    return dialog_status(2);
}
static ABI int32_t save_term(void) { return dialog_term(2); }
static ABI int32_t save_close(void) { return dialog_close(2); }
static ABI int32_t save_progress(uint32_t target, uint32_t rate) { (void)rate; return dialog_progress(2,target); }
/* OrbisSaveDataDialogResult: mode, result, buttonId, dirName* @16, param* @24, userData @32. */
static ABI int32_t save_result(unsigned char *out) {
    int32_t r=dialog_result(2,out,4);
    if (r) return r;
    memcpy(out,&save_dialog.mode,4);
    memcpy(out+4,&save_dialog.result,4);
    char *dir; unsigned char *param;
    memcpy(&dir,out+16,8); memcpy(&param,out+24,8);
    const char *name=save_dialog.dir_count && !save_dialog.result ? save_dialog.dirs[0] : "";
    if (dir) { memset(dir,0,32); memcpy(dir,name,strnlen(name,31)); }
    if (param && name[0]) runtime_savedata_param(USER_ID,name,param);
    memcpy(out+32,&save_dialog.user_data,8);
    return 0;
}
static ABI int32_t save_ready(void) { return 1; }
static ABI int32_t profile_result(void *result) { if (result) memset(result,0,4); return 0; }
/* ImeDialog: text typed on the keyboard into the game window (title bar shows it).
 * OrbisImeDialogParam: user, type, languages(8), enter label, method, filter,
 * option, max length, char16 buffer, position, alignment, placeholder, title. */
typedef struct {
    int32_t user; uint32_t type; uint64_t languages; uint32_t enter_label, input_method;
    void *filter; uint32_t option, max_length; uint16_t *buffer;
    float x, y; uint32_t halign, valign; const uint16_t *placeholder, *title; int8_t reserved[16];
} ImeParam;
static struct { int running, finished, end_status; uint16_t *buffer; uint32_t max_length; } ime;
static size_t utf16_to_utf8(const uint16_t *in, size_t limit, char *out, size_t size) {
    size_t n=0;
    for (size_t i=0; in && i<limit && in[i] && n+4<size; ++i) {
        uint32_t c=in[i];
        if (c>=0xD800 && c<0xDC00 && i+1<limit && in[i+1]>=0xDC00 && in[i+1]<0xE000) { c=0x10000+((c-0xD800)<<10)+(in[i+1]-0xDC00); ++i; }
        if (c<0x80) out[n++]=(char)c;
        else if (c<0x800) { out[n++]=(char)(0xC0|c>>6); out[n++]=(char)(0x80|(c&63)); }
        else if (c<0x10000) { out[n++]=(char)(0xE0|c>>12); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
        else { out[n++]=(char)(0xF0|c>>18); out[n++]=(char)(0x80|((c>>12)&63)); out[n++]=(char)(0x80|((c>>6)&63)); out[n++]=(char)(0x80|(c&63)); }
    }
    out[n]=0; return n;
}
static void utf8_to_utf16(const char *in, uint16_t *out, uint32_t max) {
    uint32_t n=0;
    for (const unsigned char *p=(const unsigned char *)in; *p && n<max;) {
        uint32_t c; int extra;
        if (*p<0x80) { c=*p; extra=0; } else if ((*p&0xE0)==0xC0) { c=*p&31; extra=1; }
        else if ((*p&0xF0)==0xE0) { c=*p&15; extra=2; } else { c=*p&7; extra=3; }
        ++p;
        for (int k=0;k<extra && (*p&0xC0)==0x80;++k) c=(c<<6)|(*p++&63);
        if (c>=0x10000) { if (n+2>max) break; c-=0x10000; out[n++]=(uint16_t)(0xD800+(c>>10)); out[n++]=(uint16_t)(0xDC00+(c&1023)); }
        else out[n++]=(uint16_t)c;
    }
    out[n<max ? n : max]=0;
}
static void ime_complete(int end_status, const char *text) {
    if (!end_status && ime.buffer && ime.max_length) utf8_to_utf16(text,ime.buffer,ime.max_length);
    ime.end_status=end_status; ime.finished=1; ime.running=0;
    printf("Runtime: ImeDialog %s%s%s\n",end_status ? "cancelled" : "text: ",end_status ? "" : text,"");
}
static ABI int32_t ime_init(const ImeParam *param, const void *extended) {
    (void)extended;
    if (!param || !param->buffer || !param->max_length) return (int32_t)0x80BC0004; /* IME INVALID_ADDRESS */
    if (ime.running) return (int32_t)0x80BC0003;                                  /* BUSY */
    memset(&ime,0,sizeof(ime));
    ime.buffer=param->buffer; ime.max_length=param->max_length; ime.running=1;
    char initial[512], prompt[256];
    utf16_to_utf8(param->buffer,param->max_length,initial,sizeof(initial));
    utf16_to_utf8(param->title,128,prompt,sizeof(prompt));
    const char *preset=getenv("WIPEOUT_IME_TEXT");
    if (preset) { ime_complete(0,preset); return 0; }
    /* An empty field starts with the user name: a controller alone can accept it (the port
     * has no on-screen keyboard). */
    const char *start=initial[0] ? initial : player_name();
    if (!wipeoutgpu_text_input_begin(start,prompt[0] ? prompt : "Text")) ime_complete(0,start);
    else printf("Runtime: ImeDialog opened: shown on screen; keyboard Enter or Cross accepts, Esc or Circle cancels\n");
    return 0;
}
static ABI int32_t ime_status(void) {
    if (ime.running) {
        char text[512];
        int state=wipeoutgpu_text_input_poll(text,sizeof(text));
        if (state) ime_complete(state==1 ? 0 : 1,text);
    }
    return ime.running ? 1 : ime.finished ? 2 : 0; /* Running / Finished / None */
}
static ABI int32_t ime_result(uint32_t *result) {
    if (!ime.finished) return (int32_t)0x80BC0101; /* DIALOG NOT_FINISHED */
    if (result) *result=(uint32_t)ime.end_status;
    return 0;
}
static ABI int32_t ime_term(void) { memset(&ime,0,sizeof(ime)); return 0; }

/* ---- Trophies: accepted locally, recorded in the log ---- */
static ABI int32_t trophy_context(int32_t *ctx,int32_t user,uint32_t label,uint64_t options) {
    (void)user; (void)label; (void)options;
    if (!ctx) return TROPHY_INVALID;
    *ctx=new_id(); return 0;
}
static ABI int32_t trophy_handle(int32_t *handle) { if (!handle) return TROPHY_INVALID; *handle=new_id(); return 0; }
static ABI int32_t trophy_register(int32_t ctx,int32_t handle,uint64_t options) { (void)ctx; (void)handle; (void)options; return 0; }
static ABI int32_t trophy_unlock(int32_t ctx,int32_t handle,int32_t id,int32_t *platinum) {
    (void)ctx; (void)handle;
    printf("Runtime: trophy %d unlocked\n",id);
    if (platinum) *platinum=-1;
    return 0;
}
static ABI int32_t trophy_game_info(int32_t ctx,int32_t handle,void *details,void *data) {
    (void)ctx; (void)handle;
    if (details) { uint64_t size; memcpy(&size,details,8); memset((char *)details+8,0,size>8 && size<4096 ? size-8 : 0); }
    if (data) { uint64_t size; memcpy(&size,data,8); memset((char *)data+8,0,size>8 && size<4096 ? size-8 : 0); }
    return 0;
}
static ABI int32_t trophy_info(int32_t ctx,int32_t handle,int32_t id,void *details,void *data) {
    (void)id; return trophy_game_info(ctx,handle,details,data);
}
/* Nothing is unlocked on this console: an empty 128-trophy flag set. */
static ABI int32_t trophy_unlock_state(int32_t ctx,int32_t handle,uint32_t *flags,uint32_t *count) {
    if (!flags || !count) return TROPHY_INVALID;
    if (ctx<=0 || handle<=0) return TROPHY_INVALID;
    memset(flags,0,16); *count=0;
    return 0;
}
static ABI int32_t trophy_release(int32_t id) { return id>0 ? 0 : TROPHY_INVALID; }

/* ---- PlayGo: fully installed package ---- */
static int playgo_handle, playgo_chunks=-1;
static ABI int32_t playgo_init(const void *params) {
    (void)params;
    int fd=(int)runtime_file_open("/app0/sce_sys/playgo-chunk.dat",0,0);
    unsigned char header[12];
    playgo_chunks=1;
    if (fd>=0) {
        if (runtime_file_read(fd,header,sizeof(header))==sizeof(header)) playgo_chunks=header[10]|header[11]<<8;
        runtime_file_close(fd);
    }
    printf("Runtime: PlayGo initialized; %d chunks, all installed locally\n",playgo_chunks);
    return 0;
}
static ABI int32_t playgo_open(int32_t *handle,const void *param) {
    (void)param;
    if (!handle) return PLAYGO_BAD_POINTER;
    playgo_handle=1; *handle=1; return 0;
}
static ABI int32_t playgo_chunk_ids(int32_t handle,uint16_t *ids,uint32_t count,uint32_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!out) return PLAYGO_BAD_POINTER;
    if (ids && !count) return PLAYGO_BAD_SIZE;
    if (!ids) { *out=(uint32_t)playgo_chunks; return 0; }
    uint32_t n=count<(uint32_t)playgo_chunks ? count : (uint32_t)playgo_chunks;
    for (uint32_t i=0;i<n;++i) ids[i]=(uint16_t)i;
    *out=n; return 0;
}
static ABI int32_t playgo_locus(int32_t handle,const uint16_t *ids,uint32_t count,int8_t *loci) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !loci) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) {
        if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
        loci[i]=3; /* LocalFast */
    }
    return 0;
}
static ABI int32_t playgo_speed(int32_t handle,int32_t speed) { (void)speed; return handle==playgo_handle && handle ? 0 : PLAYGO_BAD_HANDLE; }
static ABI int32_t playgo_get_speed(int32_t handle,int32_t *speed) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!speed) return PLAYGO_BAD_POINTER;
    *speed=2; return 0; /* Full: nothing left to install */
}
/* Progress of installed chunks: done (1 of 1, never a zero total to divide by). */
static ABI int32_t playgo_progress(int32_t handle,const uint16_t *ids,uint32_t count,uint64_t *out) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!ids || !out) return PLAYGO_BAD_POINTER;
    if (!count) return PLAYGO_BAD_SIZE;
    for (uint32_t i=0;i<count;++i) if (ids[i]>=playgo_chunks) return PLAYGO_BAD_CHUNK;
    out[0]=out[1]=1; return 0;
}
static ABI int32_t playgo_todo(int32_t handle,const void *list,uint32_t count) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    if (!list) return PLAYGO_BAD_POINTER;
    return count ? 0 : PLAYGO_BAD_SIZE; /* everything is installed: nothing to schedule */
}
static ABI int32_t playgo_close(int32_t handle) {
    if (handle!=playgo_handle || !handle) return PLAYGO_BAD_HANDLE;
    playgo_handle=0; return 0;
}
static ABI int32_t playgo_terminate(void) { playgo_handle=0; return 0; }

/* ---- DiscMap: the game is fully installed, no disc bitmap exists ---- */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)
static ABI int32_t discmap_on_hdd(const char *path,int64_t offset,int64_t size,int32_t *result) {
    (void)path; (void)offset; (void)size; (void)result; return DISC_MAP_NO_BITMAP;
}
static ABI int32_t discmap_8a82(const char *path,int64_t offset,int64_t size,int32_t *flags,int32_t *r1,int32_t *r2) {
    (void)path; (void)offset; (void)size; (void)flags; (void)r1; (void)r2; return DISC_MAP_NO_BITMAP;
}

/* ---- Devices that are absent: mouse, microphone, voice chat ---- */
/* A mouse port opens but never reports a connected device. */
static ABI int32_t mouse_open(int32_t user,int32_t type,int32_t index,const void *param) { (void)user; (void)type; (void)index; (void)param; return new_id(); }
static ABI int32_t mouse_read(int32_t handle,unsigned char *data,int32_t count) {
    (void)handle;
    if (!data || count<1) return (int32_t)0x80DF0001;
    memset(data,0,40); return 1; /* timestamp 0, connected=false */
}
static ABI int32_t mouse_close(int32_t handle) { (void)handle; return 0; }
static ABI int32_t audio_in_open(void) { return AUDIO_IN_NOT_OPENED; }


/* Traced stubs (WIPEOUT_NET_TRACE=1 prints the name): the PSN services online play does not provide. */
static ABI int32_t T_sceSslInit(void) { runtime_online_log("sceSslInit (stub -> lib_init_id)"); return new_id(); }
static ABI int32_t T_sceSslTerm(void) { runtime_online_log("sceSslTerm (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpInit(void) { runtime_online_log("sceHttpInit (stub -> lib_init_id)"); return new_id(); }
static ABI int32_t T_sceHttpTerm(void) { runtime_online_log("sceHttpTerm (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpDeleteTemplate(void) { runtime_online_log("sceHttpDeleteTemplate (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpSendRequest(void) { runtime_online_log("sceHttpSendRequest (stub -> http_fail)"); return HTTP_NETWORK; }
static ABI int32_t T_sceHttpSetNonblock(void) { runtime_online_log("sceHttpSetNonblock (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpSetConnectTimeOut(void) { runtime_online_log("sceHttpSetConnectTimeOut (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpsEnableOption(void) { runtime_online_log("sceHttpsEnableOption (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpsDisableOption(void) { runtime_online_log("sceHttpsDisableOption (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpSetRequestContentLength(void) { runtime_online_log("sceHttpSetRequestContentLength (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpDeleteConnection(void) { runtime_online_log("sceHttpDeleteConnection (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpDeleteRequest(void) { runtime_online_log("sceHttpDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpAbortWaitRequest(void) { runtime_online_log("sceHttpAbortWaitRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpDestroyEpoll(void) { runtime_online_log("sceHttpDestroyEpoll (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpSetEpoll(void) { runtime_online_log("sceHttpSetEpoll (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpUnsetEpoll(void) { runtime_online_log("sceHttpUnsetEpoll (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceHttpGetStatusCode(void) { runtime_online_log("sceHttpGetStatusCode (stub -> http_fail)"); return HTTP_NETWORK; }
static ABI int32_t T_sceHttpGetResponseContentLength(void) { runtime_online_log("sceHttpGetResponseContentLength (stub -> http_fail)"); return HTTP_NETWORK; }
static ABI int32_t T_sceHttpReadData(void) { runtime_online_log("sceHttpReadData (stub -> http_fail)"); return HTTP_NETWORK; }
static ABI int32_t T_sceNpGetOnlineId(void) { runtime_online_log("sceNpGetOnlineId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetNpId(void) { runtime_online_log("sceNpGetNpId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpUnregisterStateCallback(void) { runtime_online_log("sceNpUnregisterStateCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpUnregisterPlusEventCallback(void) { runtime_online_log("sceNpUnregisterPlusEventCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpCheckCallback(void) { runtime_online_log("sceNpCheckCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSetNpTitleId(void) { runtime_online_log("sceNpSetNpTitleId (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpNotifyPlusFeature(void) { runtime_online_log("sceNpNotifyPlusFeature (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSetContentRestriction(void) { runtime_online_log("sceNpSetContentRestriction (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpCreateAsyncRequest(void) { runtime_online_log("sceNpCreateAsyncRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpDeleteRequest(void) { runtime_online_log("sceNpDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpAbortRequest(void) { runtime_online_log("sceNpAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpCheckNpAvailability(void) { runtime_online_log("sceNpCheckNpAvailability (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetParentalControlInfo(void) { runtime_online_log("sceNpGetParentalControlInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpCheckPlus(void) { runtime_online_log("sceNpCheckPlus (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetGamePresenceStatus(void) { runtime_online_log("sceNpGetGamePresenceStatus (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpAuthCreateAsyncRequest(void) { runtime_online_log("sceNpAuthCreateAsyncRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpAuthDeleteRequest(void) { runtime_online_log("sceNpAuthDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpAuthGetAuthorizationCode(void) { runtime_online_log("sceNpAuthGetAuthorizationCode (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpLookupCreateTitleCtx(void) { runtime_online_log("sceNpLookupCreateTitleCtx (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpLookupDeleteTitleCtx(void) { runtime_online_log("sceNpLookupDeleteTitleCtx (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpLookupCreateAsyncRequest(void) { runtime_online_log("sceNpLookupCreateAsyncRequest (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpLookupDeleteRequest(void) { runtime_online_log("sceNpLookupDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpLookupAbortRequest(void) { runtime_online_log("sceNpLookupAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpLookupNpId(void) { runtime_online_log("sceNpLookupNpId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreCreateNpTitleCtx(void) { runtime_online_log("sceNpScoreCreateNpTitleCtx (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpScoreDeleteNpTitleCtx(void) { runtime_online_log("sceNpScoreDeleteNpTitleCtx (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpScoreCreateRequest(void) { runtime_online_log("sceNpScoreCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpScoreDeleteRequest(void) { runtime_online_log("sceNpScoreDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpScoreAbortRequest(void) { runtime_online_log("sceNpScoreAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiInitialize(void) { runtime_online_log("sceNpWebApiInitialize (stub -> lib_init_id)"); return new_id(); }
static ABI int32_t T_sceNpWebApiTerminate(void) { runtime_online_log("sceNpWebApiTerminate (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiCreateContext(void) { runtime_online_log("sceNpWebApiCreateContext (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiCreateRequest(void) { runtime_online_log("sceNpWebApiCreateRequest (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiSendRequest(void) { runtime_online_log("sceNpWebApiSendRequest (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiDeleteRequest(void) { runtime_online_log("sceNpWebApiDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiAbortRequest(void) { runtime_online_log("sceNpWebApiAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiDeleteContext(void) { runtime_online_log("sceNpWebApiDeleteContext (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiReadData(void) { runtime_online_log("sceNpWebApiReadData (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiGetHttpStatusCode(void) { runtime_online_log("sceNpWebApiGetHttpStatusCode (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiGetHttpResponseHeaderValue(void) { runtime_online_log("sceNpWebApiGetHttpResponseHeaderValue (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiGetHttpResponseHeaderValueLength(void) { runtime_online_log("sceNpWebApiGetHttpResponseHeaderValueLength (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiCreatePushEventFilter(void) { runtime_online_log("sceNpWebApiCreatePushEventFilter (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiDeletePushEventFilter(void) { runtime_online_log("sceNpWebApiDeletePushEventFilter (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiRegisterPushEventCallback(void) { runtime_online_log("sceNpWebApiRegisterPushEventCallback (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiUnregisterPushEventCallback(void) { runtime_online_log("sceNpWebApiUnregisterPushEventCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiUtilityParseNpId(void) { runtime_online_log("sceNpWebApiUtilityParseNpId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2ContextStart(void) { runtime_online_log("sceNpMatching2ContextStart (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2ContextStop(void) { runtime_online_log("sceNpMatching2ContextStop (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2DestroyContext(void) { runtime_online_log("sceNpMatching2DestroyContext (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2RegisterContextCallback(void) { runtime_online_log("sceNpMatching2RegisterContextCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2RegisterLobbyEventCallback(void) { runtime_online_log("sceNpMatching2RegisterLobbyEventCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2RegisterRoomEventCallback(void) { runtime_online_log("sceNpMatching2RegisterRoomEventCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2RegisterSignalingCallback(void) { runtime_online_log("sceNpMatching2RegisterSignalingCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2SetDefaultRequestOptParam(void) { runtime_online_log("sceNpMatching2SetDefaultRequestOptParam (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2CreateJoinRoom(void) { runtime_online_log("sceNpMatching2CreateJoinRoom (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2JoinRoom(void) { runtime_online_log("sceNpMatching2JoinRoom (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2LeaveRoom(void) { runtime_online_log("sceNpMatching2LeaveRoom (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SearchRoom(void) { runtime_online_log("sceNpMatching2SearchRoom (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetServerId(void) { runtime_online_log("sceNpMatching2GetServerId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetWorldInfoList(void) { runtime_online_log("sceNpMatching2GetWorldInfoList (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetLobbyInfoList(void) { runtime_online_log("sceNpMatching2GetLobbyInfoList (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2JoinLobby(void) { runtime_online_log("sceNpMatching2JoinLobby (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2LeaveLobby(void) { runtime_online_log("sceNpMatching2LeaveLobby (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GrantRoomOwner(void) { runtime_online_log("sceNpMatching2GrantRoomOwner (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2KickoutRoomMember(void) { runtime_online_log("sceNpMatching2KickoutRoomMember (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SetRoomDataExternal(void) { runtime_online_log("sceNpMatching2SetRoomDataExternal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SetRoomDataInternal(void) { runtime_online_log("sceNpMatching2SetRoomDataInternal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SetRoomMemberDataInternal(void) { runtime_online_log("sceNpMatching2SetRoomMemberDataInternal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SignalingGetConnectionStatus(void) { runtime_online_log("sceNpMatching2SignalingGetConnectionStatus (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SignalingGetPingInfo(void) { runtime_online_log("sceNpMatching2SignalingGetPingInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpSignalingDeleteContext(void) { runtime_online_log("sceNpSignalingDeleteContext (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSignalingActivateConnection(void) { runtime_online_log("sceNpSignalingActivateConnection (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpSignalingDeactivateConnection(void) { runtime_online_log("sceNpSignalingDeactivateConnection (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSignalingGetConnectionStatus(void) { runtime_online_log("sceNpSignalingGetConnectionStatus (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreCensorComment(void) { runtime_online_log("sceNpScoreCensorComment (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreSanitizeComment(void) { runtime_online_log("sceNpScoreSanitizeComment (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetBoardInfo(void) { runtime_online_log("sceNpScoreGetBoardInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetGameData(void) { runtime_online_log("sceNpScoreGetGameData (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByNpIdPcId(void) { runtime_online_log("sceNpScoreGetRankingByNpIdPcId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByRange(void) { runtime_online_log("sceNpScoreGetRankingByRange (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreRecordGameData(void) { runtime_online_log("sceNpScoreRecordGameData (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreRecordScore(void) { runtime_online_log("sceNpScoreRecordScore (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreSetPlayerCharacterId(void) { runtime_online_log("sceNpScoreSetPlayerCharacterId (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceDeletePort(void) { runtime_online_log("sceVoiceDeletePort (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceConnectIPortToOPort(void) { runtime_online_log("sceVoiceConnectIPortToOPort (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceDisconnectIPortFromOPort(void) { runtime_online_log("sceVoiceDisconnectIPortFromOPort (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceStart(void) { runtime_online_log("sceVoiceStart (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceStop(void) { runtime_online_log("sceVoiceStop (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2Initialize(void) { runtime_online_log("sceNpMatching2Initialize (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2Terminate(void) { runtime_online_log("sceNpMatching2Terminate (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2CreateContext(void) { runtime_online_log("sceNpMatching2CreateContext (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpRegisterStateCallbackForToolkit(void) { runtime_online_log("sceNpRegisterStateCallbackForToolkit (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpUnregisterStateCallbackForToolkit(void) { runtime_online_log("sceNpUnregisterStateCallbackForToolkit (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpCheckCallbackForLib(void) { runtime_online_log("sceNpCheckCallbackForLib (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpGetAccountIdA(void) { runtime_online_log("sceNpGetAccountIdA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetAccountCountryA(void) { runtime_online_log("sceNpGetAccountCountryA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetAccountLanguageA(void) { runtime_online_log("sceNpGetAccountLanguageA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetAccountDateOfBirthA(void) { runtime_online_log("sceNpGetAccountDateOfBirthA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetUserIdByAccountId(void) { runtime_online_log("sceNpGetUserIdByAccountId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpCheckNpAvailabilityA(void) { runtime_online_log("sceNpCheckNpAvailabilityA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpCheckNpReachability(void) { runtime_online_log("sceNpCheckNpReachability (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpGetNpReachabilityState(void) { runtime_online_log("sceNpGetNpReachabilityState (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpCreateRequest(void) { runtime_online_log("sceNpCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpInGameMessageInitialize(void) { runtime_online_log("sceNpInGameMessageInitialize (stub -> lib_init_id)"); return new_id(); }
static ABI int32_t T_sceNpInGameMessageTerminate(void) { runtime_online_log("sceNpInGameMessageTerminate (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpInGameMessageCreateHandle(void) { runtime_online_log("sceNpInGameMessageCreateHandle (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpInGameMessageDeleteHandle(void) { runtime_online_log("sceNpInGameMessageDeleteHandle (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpInGameMessagePrepareA(void) { runtime_online_log("sceNpInGameMessagePrepareA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpInGameMessageSendDataA(void) { runtime_online_log("sceNpInGameMessageSendDataA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpInGameMessageGetMemoryPoolStatistics(void) { runtime_online_log("sceNpInGameMessageGetMemoryPoolStatistics (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpAuthCreateRequest(void) { runtime_online_log("sceNpAuthCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpAuthAbortRequest(void) { runtime_online_log("sceNpAuthAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpAuthGetAuthorizationCodeA(void) { runtime_online_log("sceNpAuthGetAuthorizationCodeA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpAuthGetIdTokenA(void) { runtime_online_log("sceNpAuthGetIdTokenA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreCreateNpTitleCtxA(void) { runtime_online_log("sceNpScoreCreateNpTitleCtxA (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpScoreGetFriendsRankingA(void) { runtime_online_log("sceNpScoreGetFriendsRankingA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetFriendsRankingForCrossSave(void) { runtime_online_log("sceNpScoreGetFriendsRankingForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetGameDataByAccountId(void) { runtime_online_log("sceNpScoreGetGameDataByAccountId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByAccountId(void) { runtime_online_log("sceNpScoreGetRankingByAccountId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByAccountIdForCrossSave(void) { runtime_online_log("sceNpScoreGetRankingByAccountIdForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByAccountIdPcId(void) { runtime_online_log("sceNpScoreGetRankingByAccountIdPcId (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByAccountIdPcIdForCrossSave(void) { runtime_online_log("sceNpScoreGetRankingByAccountIdPcIdForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByRangeA(void) { runtime_online_log("sceNpScoreGetRankingByRangeA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpScoreGetRankingByRangeForCrossSave(void) { runtime_online_log("sceNpScoreGetRankingByRangeForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusCreateNpTitleCtxA(void) { runtime_online_log("sceNpTusCreateNpTitleCtxA (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpTssCreateNpTitleCtxA(void) { runtime_online_log("sceNpTssCreateNpTitleCtxA (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpTusDeleteNpTitleCtx(void) { runtime_online_log("sceNpTusDeleteNpTitleCtx (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpTusCreateRequest(void) { runtime_online_log("sceNpTusCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpTusDeleteRequest(void) { runtime_online_log("sceNpTusDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpTusAbortRequest(void) { runtime_online_log("sceNpTusAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpTssGetData(void) { runtime_online_log("sceNpTssGetData (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetDataA(void) { runtime_online_log("sceNpTusGetDataA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetDataAVUser(void) { runtime_online_log("sceNpTusGetDataAVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetDataForCrossSave(void) { runtime_online_log("sceNpTusGetDataForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetDataForCrossSaveVUser(void) { runtime_online_log("sceNpTusGetDataForCrossSaveVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusSetDataA(void) { runtime_online_log("sceNpTusSetDataA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusSetDataAVUser(void) { runtime_online_log("sceNpTusSetDataAVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusAddAndGetVariableA(void) { runtime_online_log("sceNpTusAddAndGetVariableA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusAddAndGetVariableAVUser(void) { runtime_online_log("sceNpTusAddAndGetVariableAVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusAddAndGetVariableForCrossSave(void) { runtime_online_log("sceNpTusAddAndGetVariableForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusAddAndGetVariableForCrossSaveVUser(void) { runtime_online_log("sceNpTusAddAndGetVariableForCrossSaveVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetMultiSlotVariableA(void) { runtime_online_log("sceNpTusGetMultiSlotVariableA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetMultiSlotVariableAVUser(void) { runtime_online_log("sceNpTusGetMultiSlotVariableAVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetMultiSlotVariableForCrossSave(void) { runtime_online_log("sceNpTusGetMultiSlotVariableForCrossSave (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusGetMultiSlotVariableForCrossSaveVUser(void) { runtime_online_log("sceNpTusGetMultiSlotVariableForCrossSaveVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusSetMultiSlotVariableA(void) { runtime_online_log("sceNpTusSetMultiSlotVariableA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusSetMultiSlotVariableVUser(void) { runtime_online_log("sceNpTusSetMultiSlotVariableVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusDeleteMultiSlotDataA(void) { runtime_online_log("sceNpTusDeleteMultiSlotDataA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpTusDeleteMultiSlotDataVUser(void) { runtime_online_log("sceNpTusDeleteMultiSlotDataVUser (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2CreateContextA(void) { runtime_online_log("sceNpMatching2CreateContextA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2CreateJoinRoomA(void) { runtime_online_log("sceNpMatching2CreateJoinRoomA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2JoinRoomA(void) { runtime_online_log("sceNpMatching2JoinRoomA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetUserInfoListA(void) { runtime_online_log("sceNpMatching2GetUserInfoListA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetRoomDataExternalList(void) { runtime_online_log("sceNpMatching2GetRoomDataExternalList (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetRoomDataInternal(void) { runtime_online_log("sceNpMatching2GetRoomDataInternal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetRoomMemberDataInternal(void) { runtime_online_log("sceNpMatching2GetRoomMemberDataInternal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetRoomMemberIdListLocal(void) { runtime_online_log("sceNpMatching2GetRoomMemberIdListLocal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetSignalingOptParamLocal(void) { runtime_online_log("sceNpMatching2GetSignalingOptParamLocal (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SetSignalingOptParam(void) { runtime_online_log("sceNpMatching2SetSignalingOptParam (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SetUserInfo(void) { runtime_online_log("sceNpMatching2SetUserInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SignalingGetConnectionInfoA(void) { runtime_online_log("sceNpMatching2SignalingGetConnectionInfoA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SendRoomChatMessage(void) { runtime_online_log("sceNpMatching2SendRoomChatMessage (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2SendRoomMessage(void) { runtime_online_log("sceNpMatching2SendRoomMessage (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2RegisterRoomMessageCallback(void) { runtime_online_log("sceNpMatching2RegisterRoomMessageCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpMatching2GetMemoryInfo(void) { runtime_online_log("sceNpMatching2GetMemoryInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpMatching2GetSslMemoryInfo(void) { runtime_online_log("sceNpMatching2GetSslMemoryInfo (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiCreateContextA(void) { runtime_online_log("sceNpWebApiCreateContextA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiCreateHandle(void) { runtime_online_log("sceNpWebApiCreateHandle (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpWebApiDeleteHandle(void) { runtime_online_log("sceNpWebApiDeleteHandle (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiSendRequest2(void) { runtime_online_log("sceNpWebApiSendRequest2 (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiCreateExtdPushEventFilter(void) { runtime_online_log("sceNpWebApiCreateExtdPushEventFilter (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiDeleteExtdPushEventFilter(void) { runtime_online_log("sceNpWebApiDeleteExtdPushEventFilter (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiRegisterExtdPushEventCallbackA(void) { runtime_online_log("sceNpWebApiRegisterExtdPushEventCallbackA (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWebApiUnregisterExtdPushEventCallback(void) { runtime_online_log("sceNpWebApiUnregisterExtdPushEventCallback (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWebApiGetMemoryPoolStats(void) { runtime_online_log("sceNpWebApiGetMemoryPoolStats (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpWordFilterCreateTitleCtxA(void) { runtime_online_log("sceNpWordFilterCreateTitleCtxA (stub -> np_request_ctx)"); return new_id(); }
static ABI int32_t T_sceNpWordFilterDeleteTitleCtx(void) { runtime_online_log("sceNpWordFilterDeleteTitleCtx (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWordFilterCreateRequest(void) { runtime_online_log("sceNpWordFilterCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpWordFilterDeleteRequest(void) { runtime_online_log("sceNpWordFilterDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWordFilterAbortRequest(void) { runtime_online_log("sceNpWordFilterAbortRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpWordFilterSanitizeComment(void) { runtime_online_log("sceNpWordFilterSanitizeComment (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpBandwidthTestInitStart(void) { runtime_online_log("sceNpBandwidthTestInitStart (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpBandwidthTestGetStatus(void) { runtime_online_log("sceNpBandwidthTestGetStatus (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpBandwidthTestAbort(void) { runtime_online_log("sceNpBandwidthTestAbort (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpBandwidthTestShutdown(void) { runtime_online_log("sceNpBandwidthTestShutdown (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpSnsFacebookCreateRequest(void) { runtime_online_log("sceNpSnsFacebookCreateRequest (stub -> np_request)"); return new_id(); }
static ABI int32_t T_sceNpSnsFacebookDeleteRequest(void) { runtime_online_log("sceNpSnsFacebookDeleteRequest (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSnsFacebookGetAccessToken(void) { runtime_online_log("sceNpSnsFacebookGetAccessToken (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceNpSignalingInitialize(void) { runtime_online_log("sceNpSignalingInitialize (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSignalingTerminate(void) { runtime_online_log("sceNpSignalingTerminate (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceNpSignalingCreateContext(void) { runtime_online_log("sceNpSignalingCreateContext (stub -> np_signed_out)"); return NP_SIGNED_OUT; }
static ABI int32_t T_sceVoiceInit(void) { runtime_online_log("sceVoiceInit (stub -> ok_void)"); return 0; }
static ABI int32_t T_sceVoiceEnd(void) { runtime_online_log("sceVoiceEnd (stub -> ok_void)"); return 0; }
/* HTTP objects are created so setup code proceeds; every transfer fails as unplugged. The URLs
 * and headers are traced: they show which web services the game wants. */
static ABI int32_t T_sceHttpCreateTemplate(int32_t lib,const char *agent,int32_t version,int32_t auto_proxy) {
    (void)lib; (void)version; (void)auto_proxy; runtime_online_log("sceHttpCreateTemplate(agent %s)",agent ? agent : "?"); return new_id();
}
static ABI int32_t T_sceHttpCreateConnectionWithURL(int32_t tmpl,const char *url,int32_t keepalive) {
    (void)tmpl; (void)keepalive; runtime_online_log("sceHttpCreateConnectionWithURL(%s)",url ? url : "?"); return new_id();
}
static ABI int32_t T_sceHttpCreateRequestWithURL(int32_t conn,int32_t method,const char *url,uint64_t length) {
    (void)conn; runtime_online_log("sceHttpCreateRequestWithURL(method %d, %s, %llu bytes)",method,url ? url : "?",(unsigned long long)length); return new_id();
}
static ABI int32_t T_sceHttpAddRequestHeader(int32_t request,const char *name,const char *value,uint32_t mode) {
    (void)request; (void)mode; runtime_online_log("sceHttpAddRequestHeader(%s: %s)",name ? name : "?",value ? value : "?"); return 0;
}

static const RuntimeExport exports[]={
    {"sceUserServiceInitialize",user_initialize}, {"sceUserServiceTerminate",user_terminate},
    {"sceUserServiceGetInitialUser",user_initial}, {"sceUserServiceGetLoginUserIdList",user_list},
    {"sceUserServiceGetUserName",user_name}, {"sceUserServiceGetEvent",user_event},
    {"sceSystemServiceParamGetInt",system_param}, {"sceSystemServiceGetStatus",system_status},
    {"sceSystemServiceReceiveEvent",system_event}, {"sceSystemServiceHideSplashScreen",hide_splash},
    {"sceSystemServiceLaunchWebBrowser",launch_browser},
    {"sceSystemServiceGetDisplaySafeAreaInfo",safe_area}, {"sceSystemServiceDisableMusicPlayer",disable_music_player},
    {"sceNetInit",net_init}, {"sceNetTerm",net_term}, {"sceNetErrnoLoc",net_errno_loc},
    {"sceNetPoolCreate",net_pool_create}, {"sceNetPoolDestroy",net_pool_destroy},
    {"sceNetEpollCreate",net_epoll_create}, {"sceNetEpollDestroy",net_epoll_destroy},
    {"sceNetResolverCreate",net_resolver_create}, {"sceNetResolverDestroy",net_resolver_destroy},
    {"sceNetHtons",net_htons}, {"sceNetNtohs",net_ntohs}, {"sceNetHtonl",net_htonl}, {"sceNetNtohl",net_ntohl},
    {"sceNetInetPton",net_pton}, {"sceNetInetNtop",net_ntop},
    {"sceNetSocket",net_unreachable}, {"sceNetConnect",net_unreachable}, {"sceNetBind",net_unreachable},
    {"sceNetListen",net_unreachable}, {"sceNetAccept",net_unreachable}, {"sceNetSend",net_unreachable},
    {"sceNetSendto",net_unreachable}, {"sceNetRecv",net_unreachable}, {"sceNetRecvfrom",net_unreachable},
    {"sceNetSetsockopt",net_unreachable}, {"sceNetGetsockopt",net_unreachable},
    {"sceNetGetsockname",net_unreachable}, {"sceNetShutdown",net_unreachable},
    {"sceNetSocketClose",net_unreachable}, {"sceNetSocketAbort",net_unreachable},
    {"sceNetEpollControl",net_unreachable}, {"sceNetEpollWait",net_unreachable}, {"sceNetEpollAbort",net_unreachable},
    {"sceNetResolverStartNtoa",net_unreachable}, {"sceNetResolverStartAton",net_unreachable},
    {"sceNetCtlGetState",netctl_state}, {"sceNetCtlGetInfo",netctl_info},
    {"sceNetCtlRegisterCallback",netctl_register}, {"sceNetCtlCheckCallback",netctl_check},
    {"sceNetCtlUnregisterCallback",netctl_unregister}, {"sceNetCtlGetNatInfo",netctl_nat},
    {"sceSslInit",T_sceSslInit}, {"sceSslTerm",T_sceSslTerm},
    {"sceHttpInit",T_sceHttpInit}, {"sceHttpTerm",T_sceHttpTerm},
    {"sceHttpCreateTemplate",T_sceHttpCreateTemplate}, {"sceHttpDeleteTemplate",T_sceHttpDeleteTemplate},
    {"sceHttpCreateConnectionWithURL",T_sceHttpCreateConnectionWithURL}, {"sceHttpCreateRequestWithURL",T_sceHttpCreateRequestWithURL},
    {"sceHttpSendRequest",T_sceHttpSendRequest}, {"sceHttpCreateEpoll",http_epoll},
    {"sceHttpSetNonblock",T_sceHttpSetNonblock}, {"sceHttpSetConnectTimeOut",T_sceHttpSetConnectTimeOut},
    {"sceHttpsEnableOption",T_sceHttpsEnableOption}, {"sceHttpsDisableOption",T_sceHttpsDisableOption},
    {"sceHttpAddRequestHeader",T_sceHttpAddRequestHeader}, {"sceHttpSetRequestContentLength",T_sceHttpSetRequestContentLength},
    {"sceHttpDeleteConnection",T_sceHttpDeleteConnection}, {"sceHttpDeleteRequest",T_sceHttpDeleteRequest},
    {"sceHttpAbortWaitRequest",T_sceHttpAbortWaitRequest}, {"sceHttpDestroyEpoll",T_sceHttpDestroyEpoll},
    {"sceHttpSetEpoll",T_sceHttpSetEpoll}, {"sceHttpUnsetEpoll",T_sceHttpUnsetEpoll}, {"sceHttpWaitRequest",http_wait},
    {"sceHttpGetStatusCode",T_sceHttpGetStatusCode}, {"sceHttpGetResponseContentLength",T_sceHttpGetResponseContentLength},
    {"sceHttpReadData",T_sceHttpReadData},
    {"sceNpGetState",np_state}, {"sceNpGetOnlineId",T_sceNpGetOnlineId}, {"sceNpGetNpId",T_sceNpGetNpId},
    {"sceNpRegisterStateCallback",np_register}, {"sceNpUnregisterStateCallback",T_sceNpUnregisterStateCallback},
    {"sceNpRegisterGamePresenceCallback",np_register_void}, {"sceNpRegisterPlusEventCallback",np_register},
    {"sceNpUnregisterPlusEventCallback",T_sceNpUnregisterPlusEventCallback}, {"sceNpCheckCallback",T_sceNpCheckCallback},
    {"sceNpSetNpTitleId",T_sceNpSetNpTitleId}, {"sceNpNotifyPlusFeature",T_sceNpNotifyPlusFeature}, {"sceNpSetContentRestriction",T_sceNpSetContentRestriction},
    {"sceNpCreateAsyncRequest",T_sceNpCreateAsyncRequest}, {"sceNpDeleteRequest",T_sceNpDeleteRequest}, {"sceNpAbortRequest",T_sceNpAbortRequest},
    {"sceNpPollAsync",np_poll}, {"sceNpCheckNpAvailability",T_sceNpCheckNpAvailability},
    {"sceNpGetParentalControlInfo",T_sceNpGetParentalControlInfo}, {"sceNpCheckPlus",T_sceNpCheckPlus},
    {"sceNpGetGamePresenceStatus",T_sceNpGetGamePresenceStatus},
    {"sceNpCmpNpId",np_compare}, {"sceNpCmpOnlineId",np_compare},
    {"sceNpAuthCreateAsyncRequest",T_sceNpAuthCreateAsyncRequest}, {"sceNpAuthDeleteRequest",T_sceNpAuthDeleteRequest},
    {"sceNpAuthPollAsync",np_poll}, {"sceNpAuthGetAuthorizationCode",T_sceNpAuthGetAuthorizationCode},
    {"sceNpLookupCreateTitleCtx",T_sceNpLookupCreateTitleCtx}, {"sceNpLookupDeleteTitleCtx",T_sceNpLookupDeleteTitleCtx},
    {"sceNpLookupCreateAsyncRequest",T_sceNpLookupCreateAsyncRequest}, {"sceNpLookupDeleteRequest",T_sceNpLookupDeleteRequest},
    {"sceNpLookupAbortRequest",T_sceNpLookupAbortRequest}, {"sceNpLookupPollAsync",np_poll}, {"sceNpLookupNpId",T_sceNpLookupNpId},
    {"sceNpScoreCreateNpTitleCtx",T_sceNpScoreCreateNpTitleCtx}, {"sceNpScoreDeleteNpTitleCtx",T_sceNpScoreDeleteNpTitleCtx},
    {"sceNpScoreCreateRequest",T_sceNpScoreCreateRequest}, {"sceNpScoreDeleteRequest",T_sceNpScoreDeleteRequest}, {"sceNpScoreAbortRequest",T_sceNpScoreAbortRequest},
    {"sceNpWebApiInitialize",T_sceNpWebApiInitialize}, {"sceNpWebApiTerminate",T_sceNpWebApiTerminate},
    {"sceNpWebApiCreateContext",T_sceNpWebApiCreateContext},
    {"sceNpWebApiCreateRequest",T_sceNpWebApiCreateRequest}, {"sceNpWebApiSendRequest",T_sceNpWebApiSendRequest},
    {"sceNpWebApiDeleteRequest",T_sceNpWebApiDeleteRequest}, {"sceNpWebApiAbortRequest",T_sceNpWebApiAbortRequest},
    {"sceNpWebApiDeleteContext",T_sceNpWebApiDeleteContext}, {"sceNpWebApiReadData",T_sceNpWebApiReadData},
    {"sceNpWebApiGetHttpStatusCode",T_sceNpWebApiGetHttpStatusCode}, {"sceNpWebApiGetHttpResponseHeaderValue",T_sceNpWebApiGetHttpResponseHeaderValue},
    {"sceNpWebApiGetHttpResponseHeaderValueLength",T_sceNpWebApiGetHttpResponseHeaderValueLength},
    {"sceNpWebApiCreatePushEventFilter",T_sceNpWebApiCreatePushEventFilter}, {"sceNpWebApiDeletePushEventFilter",T_sceNpWebApiDeletePushEventFilter},
    {"sceNpWebApiRegisterPushEventCallback",T_sceNpWebApiRegisterPushEventCallback}, {"sceNpWebApiUnregisterPushEventCallback",T_sceNpWebApiUnregisterPushEventCallback},
    {"sceNpWebApiUtilityParseNpId",T_sceNpWebApiUtilityParseNpId},
    {"sceNpMatching2ContextStart",T_sceNpMatching2ContextStart}, {"sceNpMatching2ContextStop",T_sceNpMatching2ContextStop},
    {"sceNpMatching2DestroyContext",T_sceNpMatching2DestroyContext},
    {"sceNpMatching2RegisterContextCallback",T_sceNpMatching2RegisterContextCallback}, {"sceNpMatching2RegisterLobbyEventCallback",T_sceNpMatching2RegisterLobbyEventCallback},
    {"sceNpMatching2RegisterRoomEventCallback",T_sceNpMatching2RegisterRoomEventCallback}, {"sceNpMatching2RegisterSignalingCallback",T_sceNpMatching2RegisterSignalingCallback},
    {"sceNpMatching2SetDefaultRequestOptParam",T_sceNpMatching2SetDefaultRequestOptParam},
    {"sceNpMatching2CreateJoinRoom",T_sceNpMatching2CreateJoinRoom}, {"sceNpMatching2JoinRoom",T_sceNpMatching2JoinRoom},
    {"sceNpMatching2LeaveRoom",T_sceNpMatching2LeaveRoom}, {"sceNpMatching2SearchRoom",T_sceNpMatching2SearchRoom},
    {"sceNpMatching2GetServerId",T_sceNpMatching2GetServerId}, {"sceNpMatching2GetWorldInfoList",T_sceNpMatching2GetWorldInfoList},
    {"sceNpMatching2GetLobbyInfoList",T_sceNpMatching2GetLobbyInfoList}, {"sceNpMatching2JoinLobby",T_sceNpMatching2JoinLobby},
    {"sceNpMatching2LeaveLobby",T_sceNpMatching2LeaveLobby}, {"sceNpMatching2GrantRoomOwner",T_sceNpMatching2GrantRoomOwner},
    {"sceNpMatching2KickoutRoomMember",T_sceNpMatching2KickoutRoomMember}, {"sceNpMatching2SetRoomDataExternal",T_sceNpMatching2SetRoomDataExternal},
    {"sceNpMatching2SetRoomDataInternal",T_sceNpMatching2SetRoomDataInternal}, {"sceNpMatching2SetRoomMemberDataInternal",T_sceNpMatching2SetRoomMemberDataInternal},
    {"sceNpMatching2SignalingGetConnectionStatus",T_sceNpMatching2SignalingGetConnectionStatus}, {"sceNpMatching2SignalingGetPingInfo",T_sceNpMatching2SignalingGetPingInfo},
    {"sceNpSignalingDeleteContext",T_sceNpSignalingDeleteContext}, {"sceNpSignalingActivateConnection",T_sceNpSignalingActivateConnection},
    {"sceNpSignalingDeactivateConnection",T_sceNpSignalingDeactivateConnection}, {"sceNpSignalingGetConnectionStatus",T_sceNpSignalingGetConnectionStatus},
    {"sceNpScoreCensorComment",T_sceNpScoreCensorComment}, {"sceNpScoreSanitizeComment",T_sceNpScoreSanitizeComment},
    {"sceNpScoreGetBoardInfo",T_sceNpScoreGetBoardInfo}, {"sceNpScoreGetGameData",T_sceNpScoreGetGameData},
    {"sceNpScoreGetRankingByNpIdPcId",T_sceNpScoreGetRankingByNpIdPcId}, {"sceNpScoreGetRankingByRange",T_sceNpScoreGetRankingByRange},
    {"sceNpScoreRecordGameData",T_sceNpScoreRecordGameData}, {"sceNpScoreRecordScore",T_sceNpScoreRecordScore},
    {"sceNpScoreSetPlayerCharacterId",T_sceNpScoreSetPlayerCharacterId},
    {"sceVoiceCreatePort",voice_port}, {"sceVoiceDeletePort",T_sceVoiceDeletePort},
    {"sceVoiceConnectIPortToOPort",T_sceVoiceConnectIPortToOPort}, {"sceVoiceDisconnectIPortFromOPort",T_sceVoiceDisconnectIPortFromOPort},
    {"sceVoiceStart",T_sceVoiceStart}, {"sceVoiceStop",T_sceVoiceStop}, {"sceVoiceGetPortInfo",voice_info},
    {"sceVoiceReadFromOPort",voice_read}, {"sceVoiceWriteToIPort",voice_write},
    {"sceAudioInInput",audio_in_open}, {"sceAudioInClose",audio_in_open},
    {"sceNpMatching2Initialize",T_sceNpMatching2Initialize}, {"sceNpMatching2Terminate",T_sceNpMatching2Terminate},
    {"sceNpMatching2CreateContext",T_sceNpMatching2CreateContext},
    /* WipEout (NpToolkit2): the toolkit's callback hooks and the account-id ("A") variants
     * answer like the calls above: offline, signed out. */
    {"sceNetCtlRegisterCallbackForNpToolkit",netctl_register}, {"sceNetCtlUnregisterCallbackForNpToolkit",netctl_unregister},
    {"sceNetCtlCheckCallbackForNpToolkit",netctl_check},
    {"sceNpRegisterStateCallbackForToolkit",T_sceNpRegisterStateCallbackForToolkit}, {"sceNpUnregisterStateCallbackForToolkit",T_sceNpUnregisterStateCallbackForToolkit},
    {"sceNpCheckCallbackForLib",T_sceNpCheckCallbackForLib},
    {"sceUserServiceRegisterEventCallback",user_register_callback},
    {"sceUserServiceUnregisterEventCallback",user_unregister_callback},
    {"sceNpGetAccountIdA",T_sceNpGetAccountIdA}, {"sceNpGetAccountCountryA",T_sceNpGetAccountCountryA},
    {"sceNpGetAccountLanguageA",T_sceNpGetAccountLanguageA}, {"sceNpGetAccountDateOfBirthA",T_sceNpGetAccountDateOfBirthA},
    {"sceNpGetUserIdByAccountId",T_sceNpGetUserIdByAccountId}, {"sceNpCheckNpAvailabilityA",T_sceNpCheckNpAvailabilityA},
    {"sceNpCheckNpReachability",T_sceNpCheckNpReachability}, {"sceNpGetNpReachabilityState",T_sceNpGetNpReachabilityState},
    {"sceNpCreateRequest",T_sceNpCreateRequest},
    {"sceNpInGameMessageInitialize",T_sceNpInGameMessageInitialize}, {"sceNpInGameMessageTerminate",T_sceNpInGameMessageTerminate},
    {"sceNpInGameMessageCreateHandle",T_sceNpInGameMessageCreateHandle}, {"sceNpInGameMessageDeleteHandle",T_sceNpInGameMessageDeleteHandle},
    {"sceNpInGameMessagePrepareA",T_sceNpInGameMessagePrepareA}, {"sceNpInGameMessageSendDataA",T_sceNpInGameMessageSendDataA},
    {"sceNpInGameMessageGetMemoryPoolStatistics",T_sceNpInGameMessageGetMemoryPoolStatistics},
    {"sceNpAuthCreateRequest",T_sceNpAuthCreateRequest}, {"sceNpAuthAbortRequest",T_sceNpAuthAbortRequest},
    {"sceNpAuthGetAuthorizationCodeA",T_sceNpAuthGetAuthorizationCodeA}, {"sceNpAuthGetIdTokenA",T_sceNpAuthGetIdTokenA},
    {"sceNpScoreCreateNpTitleCtxA",T_sceNpScoreCreateNpTitleCtxA},
    {"sceNpScoreGetFriendsRankingA",T_sceNpScoreGetFriendsRankingA}, {"sceNpScoreGetFriendsRankingForCrossSave",T_sceNpScoreGetFriendsRankingForCrossSave},
    {"sceNpScoreGetGameDataByAccountId",T_sceNpScoreGetGameDataByAccountId}, {"sceNpScoreGetRankingByAccountId",T_sceNpScoreGetRankingByAccountId},
    {"sceNpScoreGetRankingByAccountIdForCrossSave",T_sceNpScoreGetRankingByAccountIdForCrossSave}, {"sceNpScoreGetRankingByAccountIdPcId",T_sceNpScoreGetRankingByAccountIdPcId},
    {"sceNpScoreGetRankingByAccountIdPcIdForCrossSave",T_sceNpScoreGetRankingByAccountIdPcIdForCrossSave}, {"sceNpScoreGetRankingByRangeA",T_sceNpScoreGetRankingByRangeA},
    {"sceNpScoreGetRankingByRangeForCrossSave",T_sceNpScoreGetRankingByRangeForCrossSave},
    {"sceNpTusCreateNpTitleCtxA",T_sceNpTusCreateNpTitleCtxA}, {"sceNpTssCreateNpTitleCtxA",T_sceNpTssCreateNpTitleCtxA},
    {"sceNpTusDeleteNpTitleCtx",T_sceNpTusDeleteNpTitleCtx}, {"sceNpTusCreateRequest",T_sceNpTusCreateRequest},
    {"sceNpTusDeleteRequest",T_sceNpTusDeleteRequest}, {"sceNpTusAbortRequest",T_sceNpTusAbortRequest},
    {"sceNpTssGetData",T_sceNpTssGetData}, {"sceNpTusGetDataA",T_sceNpTusGetDataA}, {"sceNpTusGetDataAVUser",T_sceNpTusGetDataAVUser},
    {"sceNpTusGetDataForCrossSave",T_sceNpTusGetDataForCrossSave}, {"sceNpTusGetDataForCrossSaveVUser",T_sceNpTusGetDataForCrossSaveVUser},
    {"sceNpTusSetDataA",T_sceNpTusSetDataA}, {"sceNpTusSetDataAVUser",T_sceNpTusSetDataAVUser},
    {"sceNpTusAddAndGetVariableA",T_sceNpTusAddAndGetVariableA}, {"sceNpTusAddAndGetVariableAVUser",T_sceNpTusAddAndGetVariableAVUser},
    {"sceNpTusAddAndGetVariableForCrossSave",T_sceNpTusAddAndGetVariableForCrossSave}, {"sceNpTusAddAndGetVariableForCrossSaveVUser",T_sceNpTusAddAndGetVariableForCrossSaveVUser},
    {"sceNpTusGetMultiSlotVariableA",T_sceNpTusGetMultiSlotVariableA}, {"sceNpTusGetMultiSlotVariableAVUser",T_sceNpTusGetMultiSlotVariableAVUser},
    {"sceNpTusGetMultiSlotVariableForCrossSave",T_sceNpTusGetMultiSlotVariableForCrossSave}, {"sceNpTusGetMultiSlotVariableForCrossSaveVUser",T_sceNpTusGetMultiSlotVariableForCrossSaveVUser},
    {"sceNpTusSetMultiSlotVariableA",T_sceNpTusSetMultiSlotVariableA}, {"sceNpTusSetMultiSlotVariableVUser",T_sceNpTusSetMultiSlotVariableVUser},
    {"sceNpTusDeleteMultiSlotDataA",T_sceNpTusDeleteMultiSlotDataA}, {"sceNpTusDeleteMultiSlotDataVUser",T_sceNpTusDeleteMultiSlotDataVUser},
    {"sceNpMatching2CreateContextA",T_sceNpMatching2CreateContextA}, {"sceNpMatching2CreateJoinRoomA",T_sceNpMatching2CreateJoinRoomA},
    {"sceNpMatching2JoinRoomA",T_sceNpMatching2JoinRoomA}, {"sceNpMatching2GetUserInfoListA",T_sceNpMatching2GetUserInfoListA},
    {"sceNpMatching2GetRoomDataExternalList",T_sceNpMatching2GetRoomDataExternalList}, {"sceNpMatching2GetRoomDataInternal",T_sceNpMatching2GetRoomDataInternal},
    {"sceNpMatching2GetRoomMemberDataInternal",T_sceNpMatching2GetRoomMemberDataInternal}, {"sceNpMatching2GetRoomMemberIdListLocal",T_sceNpMatching2GetRoomMemberIdListLocal},
    {"sceNpMatching2GetSignalingOptParamLocal",T_sceNpMatching2GetSignalingOptParamLocal}, {"sceNpMatching2SetSignalingOptParam",T_sceNpMatching2SetSignalingOptParam},
    {"sceNpMatching2SetUserInfo",T_sceNpMatching2SetUserInfo}, {"sceNpMatching2SignalingGetConnectionInfoA",T_sceNpMatching2SignalingGetConnectionInfoA},
    {"sceNpMatching2SendRoomChatMessage",T_sceNpMatching2SendRoomChatMessage}, {"sceNpMatching2SendRoomMessage",T_sceNpMatching2SendRoomMessage},
    {"sceNpMatching2RegisterRoomMessageCallback",T_sceNpMatching2RegisterRoomMessageCallback},
    {"sceNpMatching2GetMemoryInfo",T_sceNpMatching2GetMemoryInfo}, {"sceNpMatching2GetSslMemoryInfo",T_sceNpMatching2GetSslMemoryInfo},
    {"sceNpWebApiCreateContextA",T_sceNpWebApiCreateContextA}, {"sceNpWebApiCreateHandle",T_sceNpWebApiCreateHandle},
    {"sceNpWebApiDeleteHandle",T_sceNpWebApiDeleteHandle}, {"sceNpWebApiSendRequest2",T_sceNpWebApiSendRequest2},
    {"sceNpWebApiCreateExtdPushEventFilter",T_sceNpWebApiCreateExtdPushEventFilter}, {"sceNpWebApiDeleteExtdPushEventFilter",T_sceNpWebApiDeleteExtdPushEventFilter},
    {"sceNpWebApiRegisterExtdPushEventCallbackA",T_sceNpWebApiRegisterExtdPushEventCallbackA}, {"sceNpWebApiUnregisterExtdPushEventCallback",T_sceNpWebApiUnregisterExtdPushEventCallback},
    {"sceNpWebApiGetMemoryPoolStats",T_sceNpWebApiGetMemoryPoolStats},
    {"sceNpWordFilterCreateTitleCtxA",T_sceNpWordFilterCreateTitleCtxA}, {"sceNpWordFilterDeleteTitleCtx",T_sceNpWordFilterDeleteTitleCtx},
    {"sceNpWordFilterCreateRequest",T_sceNpWordFilterCreateRequest}, {"sceNpWordFilterDeleteRequest",T_sceNpWordFilterDeleteRequest},
    {"sceNpWordFilterAbortRequest",T_sceNpWordFilterAbortRequest}, {"sceNpWordFilterSanitizeComment",T_sceNpWordFilterSanitizeComment},
    {"sceNpBandwidthTestInitStart",T_sceNpBandwidthTestInitStart}, {"sceNpBandwidthTestGetStatus",T_sceNpBandwidthTestGetStatus},
    {"sceNpBandwidthTestAbort",T_sceNpBandwidthTestAbort}, {"sceNpBandwidthTestShutdown",T_sceNpBandwidthTestShutdown},
    {"sceNpSnsFacebookCreateRequest",T_sceNpSnsFacebookCreateRequest}, {"sceNpSnsFacebookDeleteRequest",T_sceNpSnsFacebookDeleteRequest},
    {"sceNpSnsFacebookGetAccessToken",T_sceNpSnsFacebookGetAccessToken},
    {"sceNpSignalingInitialize",T_sceNpSignalingInitialize}, {"sceNpSignalingTerminate",T_sceNpSignalingTerminate},
    {"sceNpSignalingCreateContext",T_sceNpSignalingCreateContext},
    {"sceCommonDialogInitialize",common_init},
    {"sceMsgDialogInitialize",msg_init}, {"sceMsgDialogOpen",msg_open},
    {"sceMsgDialogUpdateStatus",msg_status}, {"sceMsgDialogTerminate",msg_term},
    {"sceMsgDialogClose",msg_close}, {"sceMsgDialogProgressBarSetValue",msg_progress},
    {"sceMsgDialogGetResult",msg_result}, {"sceMsgDialogGetStatus",msg_status},
    {"sceSaveDataDialogInitialize",save_init}, {"sceSaveDataDialogOpen",save_open},
    {"sceSaveDataDialogUpdateStatus",save_status}, {"sceSaveDataDialogTerminate",save_term},
    {"sceSaveDataDialogGetStatus",save_status}, {"sceSaveDataDialogClose",save_close},
    {"sceSaveDataDialogProgressBarSetValue",save_progress}, {"sceSaveDataDialogGetResult",save_result},
    {"sceSaveDataDialogIsReadyToDisplay",save_ready},
    {"sceNpProfileDialogInitialize",profile_init}, {"sceNpProfileDialogOpen",profile_open},
    {"sceNpProfileDialogUpdateStatus",profile_status}, {"sceNpProfileDialogTerminate",profile_term},
    {"sceNpProfileDialogGetResult",profile_result},
    {"sceNpCommerceDialogInitialize",commerce_init}, {"sceNpCommerceDialogOpen",commerce_open},
    {"sceNpCommerceDialogUpdateStatus",commerce_status}, {"sceNpCommerceDialogTerminate",commerce_term},
    {"sceImeDialogInit",ime_init}, {"sceImeDialogGetStatus",ime_status}, {"sceImeDialogGetResult",ime_result},
    {"sceImeDialogTerm",ime_term}, {"sceImeDialogAbort",ime_term},
    {"sceNpTrophyCreateContext",trophy_context}, {"sceNpTrophyCreateHandle",trophy_handle},
    {"sceNpTrophyRegisterContext",trophy_register}, {"sceNpTrophyUnlockTrophy",trophy_unlock},
    {"sceNpTrophyGetGameInfo",trophy_game_info}, {"sceNpTrophyGetTrophyInfo",trophy_info},
    {"sceNpTrophyGetTrophyUnlockState",trophy_unlock_state}, {"sceNpTrophyAbortHandle",trophy_release},
    {"sceNpTrophyDestroyHandle",trophy_release}, {"sceNpTrophyDestroyContext",trophy_release},
    {"scePlayGoInitialize",playgo_init}, {"scePlayGoOpen",playgo_open}, {"scePlayGoGetChunkId",playgo_chunk_ids},
    {"scePlayGoGetLocus",playgo_locus}, {"scePlayGoSetInstallSpeed",playgo_speed},
    {"scePlayGoGetInstallSpeed",playgo_get_speed}, {"scePlayGoGetProgress",playgo_progress},
    {"scePlayGoSetToDoList",playgo_todo}, {"scePlayGoClose",playgo_close}, {"scePlayGoTerminate",playgo_terminate},
    {"sceMouseInit",ok_void}, {"sceMouseOpen",mouse_open}, {"sceMouseRead",mouse_read}, {"sceMouseClose",mouse_close},
    {"sceAudioInOpen",audio_in_open},
    {"sceDiscMapIsRequestOnHDD",discmap_on_hdd}, {"sceDiscMap_8A828CAEE7EDD5E9",discmap_8a82},
    {"sceVoiceInit",T_sceVoiceInit}, {"sceVoiceEnd",T_sceVoiceEnd},
};
uintptr_t runtime_services_resolve(const char *name) { return RUNTIME_LOOKUP(exports,name); }
