/* runtime_presence.c: the sound banks the game opens become the scene, track and mode in the
 * state file; rooms and songs join them; the publisher thread writes the file (never the caller),
 * nothing is written unchanged or before a file is set. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define sleep_ms(ms) Sleep(ms)
#else
#include <time.h>
static void sleep_ms(unsigned ms) { struct timespec t = {ms / 1000, (long)(ms % 1000) * 1000000L}; nanosleep(&t, NULL); }
#endif

static int failed;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failed = 1; } } while (0)

static char path[1024];
static char *read_file(void) {
    static char buffer[4096];
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    size_t n = fread(buffer, 1, sizeof(buffer) - 1, f);
    fclose(f);
    buffer[n] = 0;
    return buffer;
}
/* The state is current at once; the file follows on the publisher thread (polled for up to 3 s). */
static void expect(const char *json) {
    char now[4096];
    runtime_presence_json(now, sizeof(now));
    if (strcmp(now, json)) { printf("FAIL state: %s\n  wanted: %s\n", now, json); failed = 1; }
    char line[4096];
    snprintf(line, sizeof(line), "%s\n", json);
    for (int i = 0; i < 300; ++i) {
        const char *file = read_file();
        if (file && !strcmp(file, line)) return;
        sleep_ms(10);
    }
    const char *file = read_file();
    printf("FAIL file: %s  wanted: %s", file ? file : "(none)\n", line);
    failed = 1;
}
static void expect_writes(unsigned n) {
    for (int i = 0; i < 300 && runtime_presence_writes() < n; ++i) sleep_ms(10);
    if (runtime_presence_writes() != n) { printf("FAIL writes: %u, wanted %u\n", runtime_presence_writes(), n); failed = 1; }
}

int main(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir) dir = getenv("TEMP");
    if (!dir) dir = "/tmp";
    snprintf(path, sizeof(path), "%s/gow3-presence-test-%ld.json", dir, (long)rand());
    remove(path);
    /* Before a file is set, nothing is written. */
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk");
    runtime_presence_room(7, 2, 8);
    runtime_presence_song("x");
    runtime_presence_flush();
    CHECK(runtime_presence_writes() == 0);
    CHECK(read_file() == NULL);

    /* The file exists as soon as it is configured, with the state so far. */
    runtime_presence_configure(path);
    expect_writes(1);
    expect("{\"scene\":\"\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/english(US)/Init.bnk");      /* no scene in it */
    runtime_presence_file("/app0/data/fe/newimages/demo/woc_logo.gnf");         /* not a bank */
    runtime_presence_flush();
    sleep_ms(300);
    CHECK(runtime_presence_writes() == 1);
    /* The publisher thread writes a change by itself. */
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk");
    expect("{\"scene\":\"menu\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    expect_writes(2);
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk"); /* unchanged: no write */
    runtime_presence_flush();
    sleep_ms(300);
    CHECK(runtime_presence_writes() == 2);

    /* A race: the environment bank names the track, the speech or weapons bank the mode. */
    runtime_presence_file("/app0/data/audio/sound/english(US)/env1_vinetak.bnk");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"vinetak\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/Speech_Zone.bnk");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"vinetak\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk"); /* back in the menus: all cleared */
    runtime_presence_flush();
    expect("{\"scene\":\"menu\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/speech_elim.bnk");                 /* the mode may load first */
    runtime_presence_file("/app0/data/audio/sound/env9_talonsjunction.bnk");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"talonsjunction\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env_altima.bnk");                  /* 2048 banks have no number */
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"altima\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/environments/05_ubermall/envubermall.bnk");    /* a bank in the track's folder */
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"ubermall\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env0_det.bnk");                    /* Detonator has its own environment */
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"det\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/speech_zbattle.bnk");
    runtime_presence_file("/app0/data/audio/sound/Ship_NGP_Zone.bnk");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env.bnk");                         /* no key: ignored */
    runtime_presence_file("env-odd!.bnk");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");

    /* The online room and the song, with the JSON escaped. */
    runtime_presence_room(1000, 2, 8);
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[2,8,\"1000\"],\"song\":\"\"}");
    runtime_presence_song("Artist - \"Title\"\tx\\");
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[2,8,\"1000\"],\"song\":\"Artist - \\\"Title\\\"\\u0009x\\\\\"}");
    runtime_presence_room(1000, 0, 8);
    runtime_presence_song(NULL);
    runtime_presence_flush();
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    /* Changes faster than the publisher merge: the file ends on the last one, in few writes. */
    const unsigned before = runtime_presence_writes();
    for (int i = 0; i < 50; ++i) runtime_presence_room(1000, (unsigned)(i % 8) + 1, 8);
    runtime_presence_room(1000, 5, 8);
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[5,8,\"1000\"],\"song\":\"\"}");
    sleep_ms(300);
    CHECK(runtime_presence_writes() - before <= 3);
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(fopen(tmp, "rb") == NULL); /* the temporary file is gone after each rename */
    remove(path);
    puts(failed ? "presence test: FAILED" : "presence test: OK");
    return failed;
}
