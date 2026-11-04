/* wipeout-server: the match server for WipEout online play, run on its own.
 *
 *   wipeout-server [--port N] [--quiet]
 *
 * Players put this machine's address and the port in the launcher (Online page) or in
 * WIPEOUT_NET_SERVER=host:port. The port is used for TCP (rooms) and UDP (the relay of the players'
 * game packets): forward both to this machine on the router. Ctrl+C stops it. */
#include "../src/runtime_matchserver.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

static volatile int stop_requested;
static void on_signal(int signal) { (void)signal; stop_requested = 1; }

int main(int argc, char **argv) {
    unsigned port = 0; int quiet = 0;
    for (int i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (unsigned)atoi(argv[++i]);
        else if (!strncmp(argv[i], "--port=", 7)) port = (unsigned)atoi(argv[i] + 7);
        else if (!strcmp(argv[i], "--quiet")) quiet = 1;
        else { fprintf(stderr, "Usage: %s [--port N] [--quiet]\n", argv[0]); return 2; }
    }
    if (port > 65535) { fprintf(stderr, "Port out of range\n"); return 2; }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    runtime_matchserver_set_quiet(quiet);
    if (runtime_matchserver_start((uint16_t)port)) return 1;
    puts("Players join with this machine's address and port (TCP and UDP). Ctrl+C stops the server.");
    fflush(stdout);
    unsigned last_online = 0, last_rooms = 0;
    while (!stop_requested) {
#ifdef _WIN32
        Sleep(1000);
#else
        sleep(1);
#endif
        unsigned online, rooms;
        runtime_matchserver_stats(NULL, NULL, NULL, &online, &rooms);
        if (!quiet && (online != last_online || rooms != last_rooms)) {
            printf("Server: %u player%s online, %u room%s\n", online, online == 1 ? "" : "s", rooms, rooms == 1 ? "" : "s");
            fflush(stdout);
            last_online = online; last_rooms = rooms;
        }
    }
    runtime_matchserver_stop();
    return 0;
}
