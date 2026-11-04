// SPDX-License-Identifier: GPL-2.0-or-later
// Draw-preparation worker count by hardware threads (vk_prep_workers.h). No device needed.
#include <cassert>
#include <cstdio>

#include "video_core/renderer_vulkan/vk_prep_workers.h"

using Vulkan::PrepWorkerCount;

int main() {
    // Half the hardware threads on big machines, at most 8.
    assert(PrepWorkerCount(16, nullptr) == 8);
    assert(PrepWorkerCount(12, nullptr) == 6);
    assert(PrepWorkerCount(32, nullptr) == 8);
    // None on the Steam Deck's 8 threads and below: without them the pack start ran 50-51 FPS
    // there, against 38-40 FPS with 4 workers.
    assert(PrepWorkerCount(8, nullptr) == 0);
    assert(PrepWorkerCount(4, nullptr) == 0);
    // WIPEOUT_PREP_WORKERS wins, clamped to 0..16.
    assert(PrepWorkerCount(8, "2") == 2);
    assert(PrepWorkerCount(16, "0") == 0);
    assert(PrepWorkerCount(16, "99") == 16);
    assert(PrepWorkerCount(16, "") == 8);
    std::puts("prep workers: ok");
}
