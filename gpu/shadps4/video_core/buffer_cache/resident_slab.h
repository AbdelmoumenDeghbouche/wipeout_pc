// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: where the backing of a newly resident arena range goes (BufferCache::EnsureResident).
// The backing is taken from the driver one slab at a time and handed out front to back. It is
// never freed, so a slab only needs its fill level.

#pragma once

#include <algorithm>
#include <cstdint>

namespace VideoCore {

struct ResidentSlab {
    struct Piece {
        std::uint64_t offset; ///< where the piece starts in its slab
        std::uint64_t bytes;  ///< its size: the request, or what was left of the slab
        bool new_slab;        ///< the piece is the first of a slab the caller must allocate
    };

    std::uint64_t size{}; ///< bytes per slab
    std::uint64_t free{}; ///< bytes left in the current slab

    /// The next piece for a request of `bytes` (more than 0). A request larger than what is left
    /// gets what is left; the caller asks again for the rest, which starts a new slab.
    constexpr Piece Take(std::uint64_t bytes) {
        const bool new_slab = free == 0;
        if (new_slab) {
            free = size;
        }
        const Piece piece{size - free, std::min(bytes, free), new_slab};
        free -= piece.bytes;
        return piece;
    }
};

} // namespace VideoCore
