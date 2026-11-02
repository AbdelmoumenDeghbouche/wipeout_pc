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
    // The clock runs.
    const u64 first = CompilePolicy::Now();
    assert(CompilePolicy::Now() >= first);
    std::puts("compile policy: ok");
    return EXIT_SUCCESS;
}
