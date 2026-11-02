// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: GNM occlusion queries (PIXEL_PIPE_STAT_DUMP, #109). DrawCommandBuffer::writeOcclusionQuery
// dumps the Z-pass counters into a 16-byte aligned OcclusionQueryResults: at its base for the
// begin counts, at base + 8 for the end counts, one {begin, end} pair of u64 per render backend,
// bit 63 of each meaning "written". This header classifies a dump by its address and formats
// the frame analyzer's note for it; the Vulkan query pool behind the dumps comes next.

#pragma once

#include <cstdio>

#include "common/types.h"

namespace Vulkan::Occlusion {

enum class DumpKind : u8 { Begin, End, Unaligned };

/// Which half of the results a dump at `address` writes.
[[nodiscard]] constexpr DumpKind ClassifyDump(u64 address) noexcept {
    switch (address % 16) {
    case 0:
        return DumpKind::Begin;
    case 8:
        return DumpKind::End;
    default:
        return DumpKind::Unaligned;
    }
}

/// The OcclusionQueryResults a dump at `address` belongs to.
[[nodiscard]] constexpr u64 ResultsBase(u64 address) noexcept {
    return address & ~u64{15};
}

[[nodiscard]] constexpr const char* Name(DumpKind kind) noexcept {
    switch (kind) {
    case DumpKind::Begin:
        return "begin";
    case DumpKind::End:
        return "end";
    default:
        return "unaligned";
    }
}

/// The frame analyzer's line for a dump (FrameCapture::Note): the event type as the game sent
/// it, the half of the results it writes, the address and DB_COUNT_CONTROL at that point.
/// Returns what std::snprintf returns.
inline int FormatDumpNote(char* out, size_t size, u32 event_type, u64 address,
                          u32 db_count_control) {
    return std::snprintf(out, size, "\n  occlusion dump type %u %s 0x%llx count_control 0x%x",
                         event_type, Name(ClassifyDump(address)),
                         static_cast<unsigned long long>(address), db_count_control);
}

} // namespace Vulkan::Occlusion
