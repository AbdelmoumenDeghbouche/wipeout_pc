// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Available during static initialization; an empty canonical value still overrides legacy. */
static inline const char* wipeout_oc_getenv(const char* name) {
    const char* suffix;
    if (!strncmp(name, "WIPEOUT_OC_", 11)) suffix = name + 11;
    else if (!strncmp(name, "GOW3_", 5)) suffix = name + 5;
    else return getenv(name);
    char current[256], legacy[256];
    if (snprintf(current, sizeof current, "WIPEOUT_OC_%s", suffix) >= (int)sizeof current ||
        snprintf(legacy, sizeof legacy, "GOW3_%s", suffix) >= (int)sizeof legacy) return NULL;
    const char* value = getenv(current);
    return value ? value : getenv(legacy);
}
