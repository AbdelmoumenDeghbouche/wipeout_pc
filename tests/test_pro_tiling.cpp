// SPDX-License-Identifier: GPL-2.0-or-later
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/buffer.h"
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/tile_manager.h"

#undef NDEBUG
#include <cassert>

// Pipe coordinates from AMD AddrLib, SiLib::ComputePipeFromCoord (MIT):
// https://gitlab.freedesktop.org/mesa/mesa/-/blob/main/src/amd/addrlib/src/r800/siaddrlib.cpp
static u32 ExpectedPipe(u32 x, u32 y, bool pro) {
    const u32 x3 = (x >> 3) & 1, x4 = (x >> 4) & 1;
    const u32 x5 = (x >> 5) & 1, x6 = (x >> 6) & 1;
    const u32 y3 = (y >> 3) & 1, y4 = (y >> 4) & 1;
    const u32 y5 = (y >> 5) & 1, y6 = (y >> 6) & 1;
    return pro ? ((x4 ^ y3) | ((x3 ^ y4) << 1) | ((x5 ^ y6) << 2) | ((x6 ^ y5) << 3))
               : ((x3 ^ y3 ^ x4) | ((x4 ^ y4) << 1) | ((x5 ^ y5) << 2));
}

int main() {
    using namespace VideoCore;
    const char* neo = std::getenv("GOW3_NEO");
    if (Libraries::Kernel::sceKernelIsNeoMode() != (neo && *neo == '1')) {
        std::fputs("FAIL: renderer Neo mode does not match GOW3_NEO\n", stderr);
        return 1;
    }
    const bool pro_mode = neo && *neo == '1';
    Libraries::VideoOut::BufferAttributeGroup display{};
    display.attrib.pixel_format = Libraries::VideoOut::PixelFormat::A2R10G10B10;
    display.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Tile;
    display.attrib.width = display.attrib.pitch_in_pixel = 1920;
    display.attrib.height = 1080;
    const ImageInfo tiled_display(display, 0x10'0000'0000ULL);
    // 32-bit display macrotiles are 128x64 on base and 128x128 on Pro.
    const u32 display_bytes = 1920 * (pro_mode ? 1152 : 1088) * 4;
    if (tiled_display.alt_tile != pro_mode || tiled_display.guest_size != display_bytes) {
        std::fprintf(stderr, "FAIL: VideoOut Pro=%u uses alt=%u, size=%u (expected %u)\n",
                     unsigned(pro_mode), unsigned(tiled_display.alt_tile),
                     tiled_display.guest_size, display_bytes);
        return 1;
    }
    display.attrib.tiling_mode = Libraries::VideoOut::TilingMode::Linear;
    const ImageInfo linear_display(display, 0x10'0000'0000ULL);
    assert(!linear_display.alt_tile && linear_display.guest_size == 1920 * 1080 * 4);
    Vulkan::Instance instance(0, false);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    StreamBuffer params(instance, scheduler, MemoryType::HostUncached, 1 << 20);
    TileManager tiler(instance, scheduler, runtime, params);
    constexpr u32 width = 512, height = 512, bytes = width * height * 4;
    Buffer tiled(instance, 0, bytes, MemoryType::HostUncached);
    Buffer download(instance, 0, bytes, MemoryType::HostCached);
    for (u32 offset = 0; offset < bytes; offset += 4) {
        std::memcpy(tiled.mapped_data.data() + offset, &offset, sizeof(offset));
    }
    tiled.Flush(0, bytes);
    // Reuse one TileManager to catch pipelines incorrectly shared between the two layouts.
    for (bool pro : {false, true, false, true}) {
        ImageInfo info{};
        info.type = AmdGpu::ImageType::Color2D;
        info.size = {width, height, 1};
        info.pitch = width;
        info.num_bits = 32;
        info.tile_mode = AmdGpu::TileMode::Thin2DThin;
        info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
        info.props.is_tiled = 1;
        info.alt_tile = pro;
        info.UpdateSize();
        assert(info.guest_size == bytes);
        const auto [linear, offset] = tiler.DetileImage(&tiled, 0, info);
        const vk::BufferCopy copy{offset, 0, bytes};
        runtime.CopyBuffer(linear, &download, {&copy, 1});
        runtime.AccessBuffer(&download, 0, bytes, vk::PipelineStageFlagBits2::eHost,
                             vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        download.Invalidate(0, bytes);
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                u32 address;
                std::memcpy(&address, download.mapped_data.data() + (y * width + x) * 4, 4);
                const u32 pipe = (address >> 8) & (pro ? 15u : 7u);
                if (pipe != ExpectedPipe(x, y, pro)) {
                    std::fprintf(stderr, "FAIL: %s tiling at %u,%u: pipe %u, expected %u\n",
                                 pro ? "Pro" : "base", x, y, pipe, ExpectedPipe(x, y, pro));
                    return 1;
                }
            }
        }
    }
    std::puts("PASS: GPU base/Pro detiling and pipeline separation");
}
