// SPDX-License-Identifier: GPL-2.0-or-later
// wipeout: how many speculative draw-preparation workers to run. They are SCHED_IDLE and claim whole
// command buffers, so on big machines more of them only use idle cores; on the Steam Deck's 8
// hardware threads the race's pack start ran 50-51 FPS without them against 38-40 FPS with 4
// (docs/steam-deck-performance.md, 2026-11-03), so machines with at most 8 run none.
#pragma once

#include <algorithm>
#include <cstdlib>

#include "common/types.h"

namespace Vulkan {

/// `env` is WIPEOUT_PREP_WORKERS (null or empty: not set), clamped to 0..16.
inline u32 PrepWorkerCount(u32 hardware_threads, const char* env) {
    if (env && *env) {
        return static_cast<u32>(std::clamp(std::atoi(env), 0, 16));
    }
    if (hardware_threads <= 8) {
        return 0;
    }
    return std::clamp<u32>(hardware_threads / 2, 1, 8);
}

} // namespace Vulkan
