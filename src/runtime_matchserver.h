/* The match server (runtime_matchserver.c): started inside the game by WIPEOUT_NET_HOST=1, or by
 * wipeout-server on its own. Needs only runtime_wire.c beside it. */
#ifndef WIPEOUT_RUNTIME_MATCHSERVER_H
#define WIPEOUT_RUNTIME_MATCHSERVER_H
#include <stdint.h>
#include <stddef.h>
/* Listens on the TCP and UDP port (0: the default 7788) and serves from a thread; 0 or -1. */
int runtime_matchserver_start(uint16_t port);
void runtime_matchserver_stop(void);
/* 1: no "Server:" lines on stdout. */
void runtime_matchserver_set_quiet(int quiet);
/* Counters since the start, and what is up now. */
void runtime_matchserver_stats(size_t *connections, size_t *rooms, size_t *relayed, unsigned *online, unsigned *open_rooms);
#endif
