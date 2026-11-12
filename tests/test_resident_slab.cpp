// SPDX-License-Identifier: GPL-2.0-or-later
// ResidentSlab (buffer_cache/resident_slab.h): pieces of one slab do not overlap, a slab is used
// to its last byte before the next one starts, and a large request spans slabs. No device needed.
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "video_core/buffer_cache/resident_slab.h"

using VideoCore::ResidentSlab;

int main() {
    constexpr std::uint64_t Block = 16 * 1024, Slab = 8 * 1024 * 1024;
    ResidentSlab slab{.size = Slab};

    // The first request starts a slab at offset 0; the next one follows it.
    auto a = slab.Take(Block);
    assert(a.new_slab && a.offset == 0 && a.bytes == Block);
    auto b = slab.Take(3 * Block);
    assert(!b.new_slab && b.offset == Block && b.bytes == 3 * Block);

    // A request that does not fit takes what is left, then the rest opens a new slab.
    const std::uint64_t left = Slab - 4 * Block;
    auto c = slab.Take(left + 2 * Block);
    assert(!c.new_slab && c.offset == 4 * Block && c.bytes == left);
    auto d = slab.Take(2 * Block);
    assert(d.new_slab && d.offset == 0 && d.bytes == 2 * Block);

    // A request of several slabs: every slab but the last is used whole.
    ResidentSlab big{.size = Slab};
    std::uint64_t want = 2 * Slab + 5 * Block, slabs = 0, taken = 0;
    while (want) {
        const auto piece = big.Take(want);
        slabs += piece.new_slab;
        assert(piece.bytes > 0 && piece.bytes <= want && piece.offset + piece.bytes <= Slab);
        taken += piece.bytes;
        want -= piece.bytes;
    }
    assert(slabs == 3 && taken == 2 * Slab + 5 * Block && big.free == Slab - 5 * Block);

    // Many small requests: nothing is wasted and no two pieces of a slab overlap.
    ResidentSlab many{.size = Slab};
    std::vector<std::uint64_t> next_offset; // per slab, where the next piece must start
    std::uint64_t total = 0;
    for (std::uint64_t i = 0; i < 5000; ++i) {
        std::uint64_t bytes = (1 + i % 37) * Block;
        while (bytes) {
            const auto piece = many.Take(bytes);
            if (piece.new_slab) {
                assert(next_offset.empty() || next_offset.back() == Slab); // the last one was full
                next_offset.push_back(0);
            }
            assert(piece.offset == next_offset.back());
            next_offset.back() += piece.bytes;
            total += piece.bytes;
            bytes -= piece.bytes;
        }
    }
    assert(total == (next_offset.size() - 1) * Slab + next_offset.back());
    std::puts("resident-slab-test: ok");
}
