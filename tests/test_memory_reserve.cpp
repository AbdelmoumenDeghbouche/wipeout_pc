// Size rule of the device memory held from the start (vk_memory_reserve.h).
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include "video_core/renderer_vulkan/vk_memory_reserve.h"

using namespace Vulkan;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // Unset: a third of the budget in whole 128 MiB blocks, at most 1536 MiB, none under 4096.
    static_assert(MemoryReserveMiB(4312, -1) == 1408);
    assert(MemoryReserveMiB(4312, -1) == 1408);
    assert(MemoryReserveMiB(4915, -1) == 1536);
    assert(MemoryReserveMiB(6364, -1) == 1536);
    assert(MemoryReserveMiB(4095, -1) == 0);
    assert(MemoryReserveMiB(4096, -1) == 1280);
    // 0: off whatever the budget.
    assert(MemoryReserveMiB(8000, 0) == 0);
    // A number: that many MiB, rounded up to whole blocks, whatever the budget.
    assert(MemoryReserveMiB(4312, 1000) == 1024);
    assert(MemoryReserveMiB(100, 256) == 256);
    std::puts("memory reserve: ok");
    return EXIT_SUCCESS;
}
