/* Discord Rich Presence: what the game does now, for scripts/discord_presence.py (the client that
 * talks to Discord beside the game, started by run.py). Nothing here reads the game's code or
 * memory. The sound banks the game asks for name the scene: frontend_fliers.bnk loads with the
 * menus, env1_vinetak.bnk or env_altima.bnk with a race on that track, speech_zone.bnk,
 * speech_elim.bnk or weapons_det.bnk with the mode. The matching layer tells the online room
 * (runtime_matching.c) and the custom soundtrack the song (runtime_music.c).
 * The calls from the game's threads (the file-open path, the audio mix, the matching reader)
 * only update the state under a short lock and mark it dirty: one small thread, started by
 * runtime_presence_configure, writes the state as one JSON object to GOW3_PRESENCE_FILE (run.py
 * sets it while the presence is on) whenever it is dirty, at most every 250 ms, through a
 * temporary file and a rename, so the client, which polls the file once a second, never reads
 * half of it and no game or audio thread ever waits on the file. Without a file every call
 * returns at once. */
#include "runtime.h"
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

#define PUBLISH_MS 250

typedef struct {
    char scene[8];   /* "" (starting), "menu", "race" */
    char track[32];  /* the race's environment bank key: vinetak, altima, zone, ... */
    char mode[16];   /* "", zone, zbattle, elim, det */
    uint64_t room_id; unsigned members, slots; /* the online room; members 0: none */
    char song[192];  /* the custom soundtrack's song; "" while the game's music plays */
} State;

/* The state, under `lock`, which the game's threads hold only to copy a few bytes. */
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t dirty_cond=PTHREAD_COND_INITIALIZER;
static State state;
static int dirty;
static char file[1024], temp[1040];
/* The file, under `file_lock`: only the publisher thread and runtime_presence_flush take it. */
static pthread_mutex_t file_lock=PTHREAD_MUTEX_INITIALIZER;
static char last[2048]; /* what the file holds: nothing is rewritten unchanged */
static volatile unsigned writes, failures;

/* JSON string content: quotes and backslashes escaped, control characters as \u00XX. */
static void escape(const char *in, char *out, size_t size) {
    size_t n=0;
    for (; *in && n+7<size; ++in) {
        const unsigned char c=(unsigned char)*in;
        if (c=='"' || c=='\\') { out[n++]='\\'; out[n++]=(char)c; }
        else if (c<0x20) n+=(size_t)snprintf(out+n,size-n,"\\u%04x",c);
        else out[n++]=(char)c;
    }
    out[n]=0;
}
static int render(const State *s, char *out, size_t size) {
    char song[sizeof(s->song)*6+1];
    escape(s->song,song,sizeof(song));
    return snprintf(out,size,"{\"scene\":\"%s\",\"track\":\"%s\",\"mode\":\"%s\",\"room\":[%u,%u,\"%llu\"],\"song\":\"%s\"}",
                    s->scene,s->track,s->mode,s->members,s->slots,(unsigned long long)s->room_id,song);
}
static void sleep_ms(unsigned ms) {
#ifdef _WIN32
    Sleep(ms);
#else
    struct timespec t={ms/1000,(long)(ms%1000)*1000000L};
    nanosleep(&t,NULL);
#endif
}
/* Writes the file when the state differs from what it holds (under file_lock). The client reads
 * the file for a moment at a time: the rename is retried. */
static void write_state(const State *s) {
    char json[sizeof(last)];
    pthread_mutex_lock(&file_lock);
    if (render(s,json,sizeof(json))<0 || !strcmp(json,last)) { pthread_mutex_unlock(&file_lock); return; }
    FILE *f=fopen(temp,"wb");
    if (!f) { ++failures; pthread_mutex_unlock(&file_lock); return; }
    const size_t n=strlen(json);
    const int ok=fwrite(json,1,n,f)==n && fputc('\n',f)!=EOF;
    if (fclose(f) || !ok) { ++failures; remove(temp); pthread_mutex_unlock(&file_lock); return; }
    int moved=0;
    for (int attempt=0;attempt<5 && !moved;++attempt) {
#ifdef _WIN32
        moved=MoveFileExA(temp,file,MOVEFILE_REPLACE_EXISTING)!=0;
#else
        moved=rename(temp,file)==0;
#endif
        if (!moved) sleep_ms(10);
    }
    if (!moved) { ++failures; remove(temp); }
    else { memcpy(last,json,n+1); ++writes; }
    pthread_mutex_unlock(&file_lock);
}
/* Takes the dirty state (a copy, under the short lock); 0 when nothing changed. */
static int take(State *out) {
    pthread_mutex_lock(&lock);
    const int was=dirty;
    if (was) { *out=state; dirty=0; }
    pthread_mutex_unlock(&lock);
    return was;
}
static void *publisher(void *arg) {
    (void)arg;
    State copy;
    for (;;) {
        pthread_mutex_lock(&lock);
        while (!dirty) pthread_cond_wait(&dirty_cond,&lock);
        copy=state; dirty=0;
        pthread_mutex_unlock(&lock);
        write_state(&copy);
        sleep_ms(PUBLISH_MS); /* at most one write per interval: the changes meanwhile merge into the next */
    }
    return NULL;
}
static void touch(void) { /* under lock: wakes the publisher */
    dirty=1;
    pthread_cond_signal(&dirty_cond);
}

void runtime_presence_configure(const char *path) {
    if (!path || !*path) return;
    pthread_mutex_lock(&lock);
    snprintf(file,sizeof(file),"%s",path);
    snprintf(temp,sizeof(temp),"%s.tmp",path);
    const State initial=state;
    dirty=0;
    pthread_mutex_unlock(&lock);
    last[0]=0;
    write_state(&initial); /* before the game starts: the client finds the file at once */
    pthread_t thread;
    if (pthread_create(&thread,NULL,publisher,NULL)) { fprintf(stderr,"Discord presence: cannot start its thread; state not published\n"); file[0]=0; return; }
    pthread_detach(thread);
    printf("Discord presence: game state in %s\n",path);
}

void runtime_presence_flush(void) {
    State copy;
    if (file[0] && take(&copy)) write_state(&copy);
}

static int bank_key(const char *key) { /* letters, digits and underscores only: it goes into the JSON as is */
    if (!*key) return 0;
    for (; *key; ++key) if (!islower((unsigned char)*key) && !isdigit((unsigned char)*key) && *key!='_') return 0;
    return 1;
}
static void set(char *field, size_t size, const char *value) { snprintf(field,size,"%.*s",(int)size-1,value); }

void runtime_presence_file(const char *guest_path) {
    if (!file[0] || !guest_path) return;
    const char *base=strrchr(guest_path,'/');
    base=base ? base+1 : guest_path;
    char name[64];
    size_t n=0;
    for (; base[n] && n<sizeof(name)-1; ++n) name[n]=(char)tolower((unsigned char)base[n]);
    name[n]=0;
    if (base[n] || n<5 || strcmp(name+n-4,".bnk")) return; /* not a sound bank (or a name too long to be one we know) */
    name[n-4]=0; /* the stem */
    pthread_mutex_lock(&lock);
    if (!strcmp(name,"frontend_fliers")) { /* the menus: the race, its mode and track are over */
        set(state.scene,sizeof(state.scene),"menu"); set(state.track,sizeof(state.track),""); set(state.mode,sizeof(state.mode),"");
    } else if (!strncmp(name,"env",3)) { /* env1_vinetak, env_altima, env0_zone, envubermall: a race on that track */
        const char *key=name+3;
        while (isdigit((unsigned char)*key)) ++key;
        if (*key=='_') ++key;
        if (!bank_key(key)) { pthread_mutex_unlock(&lock); return; }
        set(state.scene,sizeof(state.scene),"race"); set(state.track,sizeof(state.track),key);
        if (!strcmp(key,"det")) set(state.mode,sizeof(state.mode),"det");
    } else if (strstr(name,"zbattle")) set(state.mode,sizeof(state.mode),"zbattle");  /* speech_zbattle */
    else if (strstr(name,"zone") && (!strncmp(name,"speech",6) || !strncmp(name,"weapons",7) || !strncmp(name,"ship",4)))
        set(state.mode,sizeof(state.mode),"zone");                                     /* speech_zone(_ngp), weapons_zone, ship_ngp_zone */
    else if (!strcmp(name,"speech_elim")) set(state.mode,sizeof(state.mode),"elim");
    else if (!strcmp(name,"speech_det") || !strcmp(name,"weapons_det")) set(state.mode,sizeof(state.mode),"det");
    else { pthread_mutex_unlock(&lock); return; }
    touch();
    pthread_mutex_unlock(&lock);
}

void runtime_presence_room(uint64_t room_id, unsigned members, unsigned slots) {
    if (!file[0]) return;
    pthread_mutex_lock(&lock);
    state.room_id=members ? room_id : 0; state.members=members; state.slots=members ? slots : 0;
    touch();
    pthread_mutex_unlock(&lock);
}

void runtime_presence_song(const char *text) {
    if (!file[0]) return;
    pthread_mutex_lock(&lock);
    set(state.song,sizeof(state.song),text ? text : "");
    touch();
    pthread_mutex_unlock(&lock);
}

int runtime_presence_json(char *out, size_t size) {
    pthread_mutex_lock(&lock);
    const State copy=state;
    pthread_mutex_unlock(&lock);
    return render(&copy,out,size);
}
unsigned runtime_presence_writes(void) { return writes; }
void runtime_presence_report(void) {
    if (file[0]) printf("Presence: %u state writes, %u failures\n",writes,failures);
}
