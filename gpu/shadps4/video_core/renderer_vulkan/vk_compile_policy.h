// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Vulkan {

/// gow3: where a pipeline or shader that does not exist yet is built. On the GPU thread the draw
/// waits for it; on a compile worker the draw is skipped until the build is published, which
/// shows as track pieces missing for a frame or more (WipEout, lap 1 of the first race after a
/// launch). A pipeline restored from the on-disk binary cache costs well under a millisecond and
/// a real compile tens of milliseconds, so the GPU thread builds everything itself until a
/// pipeline has to be compiled for real (a binary cache miss), which shows that this launch is
/// compiling; the builds of the next second then go to the workers. With a warm cache nothing is
/// ever skipped. Without a binary cache every build is a compile: the workers, as before.
struct CompilePolicy {
    static constexpr u64 window_ns = 1'000'000'000; ///< background builds for this long after one

    std::atomic<u64> last_compile_ns{0}; ///< Now() of the last real compile; 0: none yet

    static u64 Now() {
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        return ns > 0 ? u64(ns) : 1;
    }

    /// A pipeline was compiled rather than restored (any thread).
    void Compiled(u64 now_ns) {
        last_compile_ns.store(now_ns ? now_ns : 1, std::memory_order_relaxed);
    }

    /// `enabled`: the in-game "compile new shaders in the background" setting.
    bool Background(u64 now_ns, bool enabled = true) const {
        const u64 last = last_compile_ns.load(std::memory_order_relaxed);
        return enabled && last && now_ns - last < window_ns;
    }
};

inline CompilePolicy g_compile_policy;

} // namespace Vulkan
