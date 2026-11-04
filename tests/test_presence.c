/* runtime_presence.c: the sound banks the game opens become the scene, track and mode in the
 * state file; rooms and songs join them; nothing is written unchanged or before a file is set. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static void expect(const char *json) {
    char now[4096];
    runtime_presence_json(now, sizeof(now));
    if (strcmp(now, json)) { printf("FAIL state: %s\n  wanted: %s\n", now, json); failed = 1; }
    const char *file = read_file();
    if (!file) { printf("FAIL no state file\n"); failed = 1; return; }
    char line[4096];
    snprintf(line, sizeof(line), "%s\n", json);
    if (strcmp(file, line)) { printf("FAIL file: %s  wanted: %s", file, line); failed = 1; }
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
    CHECK(runtime_presence_writes() == 0);
    CHECK(read_file() == NULL);

    runtime_presence_configure(path);
    CHECK(runtime_presence_writes() == 1);
    expect("{\"scene\":\"\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/english(US)/Init.bnk");      /* no scene in it */
    runtime_presence_file("/app0/data/fe/newimages/demo/woc_logo.gnf");         /* not a bank */
    CHECK(runtime_presence_writes() == 1);
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk");
    expect("{\"scene\":\"menu\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk"); /* unchanged: no write */
    CHECK(runtime_presence_writes() == 2);

    /* A race: the environment bank names the track, the speech or weapons bank the mode. */
    runtime_presence_file("/app0/data/audio/sound/english(US)/env1_vinetak.bnk");
    expect("{\"scene\":\"race\",\"track\":\"vinetak\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/Speech_Zone.bnk");
    expect("{\"scene\":\"race\",\"track\":\"vinetak\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/english(US)/frontend_fliers.bnk"); /* back in the menus: all cleared */
    expect("{\"scene\":\"menu\",\"track\":\"\",\"mode\":\"\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/speech_elim.bnk");                 /* the mode may load first */
    runtime_presence_file("/app0/data/audio/sound/env9_talonsjunction.bnk");
    expect("{\"scene\":\"race\",\"track\":\"talonsjunction\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env_altima.bnk");                  /* 2048 banks have no number */
    expect("{\"scene\":\"race\",\"track\":\"altima\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/environments/05_ubermall/envubermall.bnk");    /* a bank in the track's folder */
    expect("{\"scene\":\"race\",\"track\":\"ubermall\",\"mode\":\"elim\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env0_det.bnk");                    /* Detonator has its own environment */
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"det\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/speech_zbattle.bnk");
    runtime_presence_file("/app0/data/audio/sound/Ship_NGP_Zone.bnk");
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    runtime_presence_file("/app0/data/audio/sound/env.bnk");                         /* no key: ignored */
    runtime_presence_file("env-odd!.bnk");
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");

    /* The online room and the song, with the JSON escaped. */
    runtime_presence_room(1000, 2, 8);
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[2,8,\"1000\"],\"song\":\"\"}");
    runtime_presence_song("Artist - \"Title\"\tx\\");
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[2,8,\"1000\"],\"song\":\"Artist - \\\"Title\\\"\\u0009x\\\\\"}");
    runtime_presence_room(1000, 0, 8);
    runtime_presence_song(NULL);
    expect("{\"scene\":\"race\",\"track\":\"det\",\"mode\":\"zone\",\"room\":[0,0,\"0\"],\"song\":\"\"}");
    char tmp[1100];
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    CHECK(fopen(tmp, "rb") == NULL); /* the temporary file is gone after each rename */
    remove(path);
    puts(failed ? "presence test: FAILED" : "presence test: OK");
    return failed;
}
