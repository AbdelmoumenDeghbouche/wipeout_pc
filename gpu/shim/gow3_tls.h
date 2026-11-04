// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: thread-locals read on every draw. libgow3gpu is linked at start-up (DT_NEEDED, never
// dlopen'ed), so the initial-exec model is valid and turns each access into one %fs-relative load
// instead of a __tls_get_addr call (2.2 % of the draw recording thread in a race-start profile).
#pragma once

#if defined(__ELF__)
#define GOW3_TLS_INITIAL_EXEC __attribute__((tls_model("initial-exec")))
#else
#define GOW3_TLS_INITIAL_EXEC
#endif
