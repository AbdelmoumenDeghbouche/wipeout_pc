// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Vulkan {

/// wipeout: a missing pipeline or shader is built on the GPU thread (the draw waits) unless a
/// pipeline was compiled for real in the last second, a binary cache miss; then the workers
/// build it and the draw is skipped until it is published. A restore from the cache takes well
/// under a millisecond, so with a warm cache nothing is ever skipped (WipEout lap-1 flashes).
struct CompilePolicy {
    static constexpr u64 window_ns = 1'000'000'000;

    std::atomic<u64> background_until_ns{0};

    static u64 Now() {
        return u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now().time_since_epoch())
                       .count());
    }

    /// A pipeline was compiled rather than restored (any thread).
    void Compiled(u64 now_ns) {
        background_until_ns.store(now_ns + window_ns, std::memory_order_relaxed);
    }

    /// `enabled`: the in-game "compile new shaders in the background" setting.
    bool Background(u64 now_ns, bool enabled = true) const {
        return enabled && now_ns < background_until_ns.load(std::memory_order_relaxed);
    }
};

inline CompilePolicy g_compile_policy;

} // namespace Vulkan
