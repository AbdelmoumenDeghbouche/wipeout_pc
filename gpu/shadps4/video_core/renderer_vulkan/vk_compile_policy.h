// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <atomic>
#include <chrono>
#include "common/types.h"

namespace Vulkan {

/// gow3: a missing pipeline or shader is built on the GPU thread (the draw waits) unless a
/// pipeline was compiled for real in the last second, a binary cache miss; then the workers
/// build it and the draw is skipped until it is published. A restore from the cache takes well
/// under a millisecond, so with a warm cache nothing is ever skipped (WipEout lap-1 flashes).
struct CompilePolicy {
    static constexpr u64 window_ns = 1'000'000'000;

    /// gow3: a frame with at least this many draws shows a scene; a loading screen draws about 50.
    static constexpr u64 scene_draws = 500;
    /// The longer grace wait lasts this long into the first scene after a load. With a fresh
    /// cache a race intro builds its pipelines in two bursts, from its first frame and about
    /// 5.5 s in; the racing starts about 20 s in.
    static constexpr u64 after_load_ns = 7'000'000'000;

    std::atomic<u64> background_until_ns{0};
    std::atomic<u64> long_grace_until_ns{0};
    std::atomic<u64> flip_draws{0};
    std::atomic<u32> flip_loads{0};
    std::atomic<bool> loading{true};     ///< no scene yet since the start or the last load
    std::atomic<bool> load_grace{false}; ///< set by the pipeline cache: the longer wait is in use

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

    /// Each flip; `draws` counts every draw so far, `loads` the game's loads of a race or of the
    /// menus (runtime_music_loads). True when the frame is the first scene after a load: the
    /// longer grace wait ends after_load_ns from now.
    bool Flipped(u64 now_ns, u64 draws, u32 loads) {
        const u64 frame_draws = draws - flip_draws.exchange(draws, std::memory_order_relaxed);
        if (flip_loads.exchange(loads, std::memory_order_relaxed) != loads) {
            loading.store(true, std::memory_order_relaxed);
        }
        if (frame_draws < scene_draws || !loading.exchange(false, std::memory_order_relaxed)) {
            return false;
        }
        long_grace_until_ns.store(now_ns + after_load_ns, std::memory_order_relaxed);
        return true;
    }

    /// A draw may wait longer for its pipeline (GraceWait): a loading screen, the menus before
    /// a scene, or the first seconds of a scene. Covers the first scene frame too, which is
    /// drawn before its flip and needs the most new pipelines.
    bool LongGrace(u64 now_ns) const {
        return load_grace.load(std::memory_order_relaxed) &&
               (loading.load(std::memory_order_relaxed) ||
                now_ns < long_grace_until_ns.load(std::memory_order_relaxed));
    }
};

inline CompilePolicy g_compile_policy;

} // namespace Vulkan
