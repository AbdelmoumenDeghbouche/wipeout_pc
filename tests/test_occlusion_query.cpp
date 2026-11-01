// SPDX-License-Identifier: GPL-2.0-or-later
// GNM occlusion queries (vk_occlusion_query.h): which half of a results entry a
// PIXEL_PIPE_STAT_DUMP writes, and the frame analyzer's note for it. No game or Vulkan device.
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "video_core/renderer_vulkan/vk_occlusion_query.h"

using namespace Vulkan::Occlusion;

int main() {
#ifdef _WIN32
    _set_error_mode(_OUT_TO_STDERR);
#endif
    // GNM's OcclusionQueryResults is 16-byte aligned: a dump at the base writes the begin
    // counts, one at base + 8 the end counts (DrawCommandBuffer::writeOcclusionQuery).
    assert(ClassifyDump(0x10000) == DumpKind::Begin);
    assert(ClassifyDump(0x10008) == DumpKind::End);
    assert(ClassifyDump(0x10004) == DumpKind::Unaligned);
    assert(ClassifyDump(0x1000C) == DumpKind::Unaligned);
    assert(ResultsBase(0x10008) == 0x10000);
    assert(ResultsBase(0x10000) == 0x10000);

    // The capture note names the event type, the half, the address and DB_COUNT_CONTROL.
    char note[160];
    const int n = FormatDumpNote(note, sizeof(note), 57, 0x10008, 0x2);
    assert(n > 0 && n < int(sizeof(note)));
    assert(std::strstr(note, "occlusion dump type 57 end 0x10008 count_control 0x2"));
    FormatDumpNote(note, sizeof(note), 57, 0x10000, 0x0);
    assert(std::strstr(note, "occlusion dump type 57 begin 0x10000 count_control 0x0"));
    FormatDumpNote(note, sizeof(note), 57, 0x10004, 0x0);
    assert(std::strstr(note, "occlusion dump type 57 unaligned 0x10004 count_control 0x0"));
    // One note per line of the pass block, as the other draw notes in vk_rasterizer.cpp.
    assert(note[0] == '\n');

    std::puts("occlusion-query-test: OK");
    return 0;
}
