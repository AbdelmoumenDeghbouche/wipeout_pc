// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: size of the device memory held for images from the start (Instance::CreateAllocator).

#pragma once

#include <algorithm>

#include "common/types.h"

namespace Vulkan {

/// gow3: the pool is made of blocks of this size, all at start, never grown, never returned.
constexpr u64 MEMORY_RESERVE_BLOCK_MIB = 128;

/// gow3: MiB of device memory to hold for images, from the budget of the heap images use and
/// GOW3_MEMORY_RESERVE (`requested`: -1 unset, 0 off, else MiB). Unset: a third of the budget in
/// whole blocks, at most 1536 MiB, and none under 4096 MiB: big textures leave holes in shared
/// blocks (#16, #72) and a card that small has no memory to spare for them. A number: that
/// many MiB, rounded up to whole blocks.
constexpr u64 MemoryReserveMiB(u64 budget_mib, s64 requested) {
    constexpr u64 block = MEMORY_RESERVE_BLOCK_MIB;
    if (requested == 0) {
        return 0;
    }
    if (requested > 0) {
        return (static_cast<u64>(requested) + block - 1) / block * block;
    }
    if (budget_mib < 4096) {
        return 0;
    }
    return std::min<u64>(1536, budget_mib / 3) / block * block;
}

} // namespace Vulkan
