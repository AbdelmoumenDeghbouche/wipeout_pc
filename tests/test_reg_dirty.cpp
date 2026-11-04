// SPDX-License-Identifier: GPL-2.0-or-later
// RegDirty's one-pass delivery (regs.h ForEachAndClear). No device needed.
#include <cassert>
#include <cstdio>
#include <vector>

#include "video_core/amdgpu/regs.h"

using AmdGpu::RegDirty;

int main() {
    RegDirty dirty;
    dirty.Mark(0, 1);                             // block 0
    dirty.Mark(RegDirty::BlockWords * 5, 2);      // block 5
    dirty.Mark(RegDirty::BlockWords * 5 + 1, 1);  // block 5 again
    dirty.Mark(RegDirty::BlockWords * 70, 1);     // block 70, second mask word
    dirty.reset = true;
    std::vector<unsigned> seen;
    bool reset_seen = false;
    dirty.ForEachAndClear([&](unsigned block) {
        seen.push_back(block);
        reset_seen = dirty.reset;
    });
    assert((seen == std::vector<unsigned>{0, 5, 70}));
    assert(reset_seen);
    assert(!dirty.reset);
    for (unsigned i = 0; i < RegDirty::NumMaskWords; ++i) {
        assert(dirty.blocks[i] == 0);
    }
    // Empty mask: no calls.
    unsigned calls = 0;
    dirty.ForEachAndClear([&](unsigned) { ++calls; });
    assert(calls == 0);
    // A two-word field at the last word of a block marks the next block too.
    RegDirty straddle;
    straddle.Mark(RegDirty::BlockWords - 1, 2);
    std::vector<unsigned> blocks_hit;
    straddle.ForEachAndClear([&](unsigned block) { blocks_hit.push_back(block); });
    assert((blocks_hit == std::vector<unsigned>{0, 1}));
    static_assert(RegDirty::BlockWords == 16);
    std::puts("reg dirty: ok");
}
