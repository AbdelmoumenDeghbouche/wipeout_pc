// SPDX-License-Identifier: GPL-2.0-or-later
// Whether the GPU command thread folds register packets into a checksum (liverpool_hash.h).
#include <cassert>
#include <cstdio>

#include "video_core/amdgpu/liverpool_hash.h"

int main() {
    // Only the draw-preparation workers compare the checksum; none, no hash.
    assert(!AmdGpu::HashRegisters(0));
    assert(AmdGpu::HashRegisters(1));
    assert(AmdGpu::HashRegisters(8));
    std::puts("hash registers: ok");
}
