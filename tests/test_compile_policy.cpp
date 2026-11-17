// Synchronous-or-background pipeline compile policy (vk_compile_policy.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_compile_policy.h"

using namespace Vulkan;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    constexpr u64 ms = 1'000'000;
    CompilePolicy policy;
    // Nothing compiled yet: every build on the GPU thread.
    assert(!policy.Background(0));
    assert(!policy.Background(10'000 * ms));
    // A real compile (a binary cache miss) switches to the background for a while.
    policy.Compiled(200 * ms);
    assert(policy.Background(200 * ms));
    assert(policy.Background(200 * ms + CompilePolicy::window_ns - 1));
    assert(!policy.Background(200 * ms + CompilePolicy::window_ns));
    // Another one inside the window extends it.
    policy.Compiled(200 * ms + CompilePolicy::window_ns / 2);
    assert(policy.Background(200 * ms + CompilePolicy::window_ns + CompilePolicy::window_ns / 4));
    // Switched off in the settings: never background, whatever was compiled.
    assert(!policy.Background(200 * ms + 1, false));
    // gow3: the longer grace wait after a load.
    {
        constexpr u64 s = 1'000 * ms;
        CompilePolicy load;
        u64 draws = 0;
        u32 loads = 0;
        const auto flip = [&](u64 t, u64 frame_draws) {
            draws += frame_draws;
            return load.Flipped(t, draws, loads);
        };
        // Not in use (no fast first builds): never the longer wait.
        assert(!load.LongGrace(1 * s));
        load.load_grace = true;
        // Start-up: no scene yet, so the menus and the first scene may wait longer.
        assert(load.LongGrace(1 * s));
        // The menus and a loading screen keep it; their frames start nothing.
        ++loads; // the race's sound banks
        for (u64 t = 10 * s; t < 20 * s; t += 16 * ms) {
            assert(!flip(t, 54));
        }
        assert(load.LongGrace(20 * s)); // the first scene frame is being drawn
        // The first scene frame starts the window, which ends after_load_ns later.
        assert(flip(20 * s, 3300));
        assert(load.LongGrace(20 * s + CompilePolicy::after_load_ns - 1));
        // Scene frames inside it start nothing new.
        u64 t = 20 * s;
        for (; t < 40 * s; t += 16 * ms) {
            assert(!flip(t, 3000));
        }
        assert(!load.LongGrace(40 * s)); // racing: the short budget
        // Light frames without a load (a pause menu, a fade) open no window, however long.
        for (; t < 50 * s; t += 16 * ms) {
            assert(!flip(t, 40));
        }
        assert(!load.LongGrace(t));
        assert(!flip(t, 3000));
        assert(!load.LongGrace(t));
        // The next load: the longer wait from the load on, and a window at its first scene.
        ++loads;
        assert(!flip(t + 1 * s, 54));
        assert(load.LongGrace(t + 1 * s));
        assert(flip(t + 6 * s, 3300));
        assert(load.LongGrace(t + 6 * s + CompilePolicy::after_load_ns - 1));
        assert(!load.LongGrace(t + 6 * s + CompilePolicy::after_load_ns));
    }
    // The clock runs.
    const u64 first = CompilePolicy::Now();
    assert(CompilePolicy::Now() >= first);
    std::puts("compile policy: ok");
    return EXIT_SUCCESS;
}
