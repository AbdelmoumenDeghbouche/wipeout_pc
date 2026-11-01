// SPDX-License-Identifier: GPL-2.0-or-later
// GOW3_TC_TRACE address range (texture_cache/trace_range.h): parsing the variable and testing
// whether a guest range intersects it. No game or Vulkan device.
#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "video_core/texture_cache/trace_range.h"

using namespace VideoCore;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    const auto range = ParseTraceRange("0x1038eb0000-0x1038ed0000");
    assert(range);
    assert(range->lo == 0x1038eb0000);
    assert(range->hi == 0x1038ed0000);

    assert(!ParseTraceRange("garbage"));
    assert(!ParseTraceRange(""));
    assert(!ParseTraceRange(nullptr));
    assert(!ParseTraceRange("0x10-0x10"));

    // Touching lo from below or starting at hi is outside; overlapping either edge is inside.
    constexpr TraceRange r{0x1000, 0x2000};
    static_assert(!r.Contains(0x0f00, 0x100));
    static_assert(r.Contains(0x0f00, 0x101));
    static_assert(!r.Contains(0x2000, 0x100));
    static_assert(r.Contains(0x1fff, 0x100));
    assert(r.Contains(0x1800, 0x10));

    std::puts("trace-range-test: OK");
    return 0;
}
