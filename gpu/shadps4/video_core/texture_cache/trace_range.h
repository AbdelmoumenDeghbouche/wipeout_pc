// SPDX-License-Identifier: GPL-2.0-or-later
// wipeout: the guest address range WIPEOUT_TC_TRACE names (#109). The texture cache logs its decisions
// for images and requests that intersect it while a frame capture records. The parser is pure
// (tests/test_trace_range.cpp); IsTraced is defined in texture_cache.cpp.

#pragma once

#include <charconv>
#include <cstring>
#include <optional>

#include "common/types.h"

namespace VideoCore {

struct TraceRange {
    u64 lo = 0, hi = 0;

    /// True when [address, address + size) intersects [lo, hi).
    constexpr bool Contains(u64 address, u64 size) const {
        return address < hi && address + size > lo;
    }
};

/// "0x1038eb0000-0x1038ed0000" -> {lo, hi}; nullopt for null, empty or malformed text, or hi <= lo.
inline std::optional<TraceRange> ParseTraceRange(const char* text) {
    if (!text) {
        return std::nullopt;
    }
    const char* const end = text + std::strlen(text);
    // Reads one hex number with an optional 0x prefix and returns where it stopped.
    const auto parse_hex = [end](const char* p, u64& value) -> const char* {
        if (end - p >= 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            p += 2;
        }
        const auto [stop, ec] = std::from_chars(p, end, value, 16);
        return ec == std::errc{} ? stop : nullptr;
    };
    TraceRange range;
    const char* p = parse_hex(text, range.lo);
    if (!p || p == end || *p != '-') {
        return std::nullopt;
    }
    p = parse_hex(p + 1, range.hi);
    if (!p || p != end || range.hi <= range.lo) {
        return std::nullopt;
    }
    return range;
}

/// True when WIPEOUT_TC_TRACE is set, [address, address + size) intersects its range and a frame
/// capture records (texture_cache.cpp).
bool IsTraced(u64 address, u64 size);

} // namespace VideoCore
