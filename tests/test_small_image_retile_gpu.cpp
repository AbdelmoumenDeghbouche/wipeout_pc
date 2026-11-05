// SPDX-License-Identifier: GPL-2.0-or-later
// GPU round-trip regression for small-image base and Pro tiling parameters.
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include "video_core/amdgpu/resource.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/texture_cache/blit_helper.h"
#include "video_core/texture_cache/image.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/tile_manager.h"

// Keep Vulkan-Hpp's dispatcher layout consistent with the library before enabling assertions.
#undef NDEBUG
#include <cassert>

namespace {
constexpr u32 Canary = 0xcdcdcdcd;
constexpr u32 PixelBase = 0x10000000;

struct Case {
    const char* name;
    u32 width, height, layers;
    bool tiled;
    u8 bank_swizzle;
};

u32 Word(const VideoCore::Buffer& buffer, u64 offset) {
    assert(offset + sizeof(u32) <= buffer.mapped_data.size());
    u32 value;
    std::memcpy(&value, buffer.mapped_data.data() + offset, sizeof(value));
    return value;
}

bool RunCase(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler,
             Vulkan::Runtime& runtime, VideoCore::TileManager& tiler,
             Common::SlotVector<VideoCore::ImageView>& views, const Case& test, bool pro) {
    using namespace VideoCore;
    const u32 texels = test.width * test.height * test.layers;
    assert(texels <= 64); // Exercise the real small-retile branch rather than its shader fallback.
    ImageInfo info{};
    info.type = test.layers == 1 ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DArray;
    info.pixel_format = vk::Format::eR32Uint;
    info.size = {test.width, test.height, 1};
    info.pitch = test.width;
    info.num_bits = 32;
    info.resources = {.levels = 1, .layers = test.layers};
    info.tile_mode = test.tiled ? AmdGpu::TileMode::Thin2DThin
                                : AmdGpu::TileMode::DisplayLinearAligned;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.props.is_tiled = test.tiled;
    info.alt_tile = test.tiled && pro;
    info.bank_swizzle = test.bank_swizzle;
    info.UpdateSize();
    Image image(instance, runtime, views, info);
    Buffer upload(instance, 0, texels * sizeof(u32), MemoryType::HostUncached);
    const u64 guard = std::max<u64>(256, instance.StorageMinAlignment());
    Buffer tiled(instance, 0, guard + info.guest_size + guard, MemoryType::HostCached);
    Buffer linear(instance, 0, info.guest_size, MemoryType::HostCached);
    for (u32 i = 0; i < texels; ++i) {
        const u32 pixel = PixelBase + i;
        std::memcpy(upload.mapped_data.data() + i * sizeof(u32), &pixel, sizeof(pixel));
    }
    upload.Flush(0, upload.SizeBytes());
    runtime.FillBuffer(&tiled, 0, tiled.SizeBytes(), Canary);
    const vk::ImageSubresourceLayers subresource{vk::ImageAspectFlagBits::eColor, 0, 0, test.layers};
    const vk::BufferImageCopy packed{
        .imageSubresource = subresource,
        .imageExtent = {test.width, test.height, 1},
    };
    runtime.UploadImage(&image, &upload, {&packed, 1});
    const auto& mip = info.mips_layout[0];
    std::array copies{vk::BufferImageCopy{
        .bufferRowLength = mip.pitch,
        .bufferImageHeight = mip.height,
        .imageSubresource = subresource,
        .imageExtent = {test.width, test.height, 1},
    }};
    tiler.TileImage(image, copies, &tiled, guard);
    // The general GPU detiler selects base/Pro parameters and is independently checked against
    // AMD AddrLib pipe coordinates by test_pro_tiling.cpp; the oracle here is the CPU pixel pattern.
    const auto [decoded, offset] = tiler.DetileImage(&tiled, guard, info);
    const vk::BufferCopy readback{offset, 0, info.guest_size};
    runtime.CopyBuffer(decoded, &linear, {&readback, 1});
    runtime.AccessBuffer(&tiled, 0, tiled.SizeBytes(), vk::PipelineStageFlagBits2::eHost,
                         vk::AccessFlagBits2::eHostRead);
    runtime.AccessBuffer(&linear, 0, linear.SizeBytes(), vk::PipelineStageFlagBits2::eHost,
                         vk::AccessFlagBits2::eHostRead);
    runtime.FlushBarriers();
    scheduler.Finish(); // Every resource below stays alive until its real GPU work completes.
    tiled.Invalidate(0, tiled.SizeBytes());
    linear.Invalidate(0, linear.SizeBytes());

    for (u64 at = 0; at < guard; at += sizeof(u32)) {
        if (Word(tiled, at) != Canary || Word(tiled, guard + info.guest_size + at) != Canary) {
            std::fprintf(stderr, "FAIL: %s %s wrote outside the guest image span\n",
                         pro ? "Pro" : "base", test.name);
            return false;
        }
    }
    for (u32 layer = 0; layer < test.layers; ++layer) {
        for (u32 y = 0; y < test.height; ++y) {
            for (u32 x = 0; x < test.width; ++x) {
                const u32 index = (layer * test.height + y) * test.width + x;
                const u32 at = ((layer * mip.height + y) * mip.pitch + x) * sizeof(u32);
                const u32 actual = Word(linear, at);
                if (actual != PixelBase + index) {
                    std::fprintf(stderr, "FAIL: %s %s layer %u (%u,%u): golden %#x, decoded %#x\n",
                                 pro ? "Pro" : "base", test.name, layer, x, y,
                                 PixelBase + index, actual);
                    return false;
                }
            }
        }
    }
    std::printf("PASS: %s %s (%ux%u, %u layers, swizzle %u)\n", pro ? "Pro" : "base",
                test.name, test.width, test.height, test.layers, test.bank_swizzle);
    return true;
}
} // namespace

int main() {
    Vulkan::Instance instance(0, true);
    Vulkan::Scheduler scheduler(instance);
    Vulkan::Runtime runtime(instance, scheduler);
    VideoCore::StreamBuffer params(instance, scheduler, VideoCore::MemoryType::HostUncached, 1 << 20);
    VideoCore::TileManager tiler(instance, scheduler, runtime, params);
    Common::SlotVector<VideoCore::ImageView> views;
    const std::array cases{
        Case{"vertical", 1, 64, 1, true, 0}, Case{"horizontal", 64, 1, 1, true, 0},
        Case{"array", 4, 4, 4, true, 0}, Case{"array-swizzle", 2, 2, 8, true, 3},
        Case{"1x1", 1, 1, 1, true, 0}, Case{"2x2", 2, 2, 1, true, 0},
        Case{"4x4", 4, 4, 1, true, 0}, Case{"8x8", 8, 8, 1, true, 0},
        Case{"linear-array", 4, 4, 4, false, 0},
    };
    unsigned failures = 0;
    for (bool pro : {false, true}) {
        for (const auto& test : cases) {
            failures += !RunCase(instance, scheduler, runtime, tiler, views, test, pro);
        }
    }
    std::printf("Small-image retile GPU: %u/18 cases passed\n", 18 - failures);
    return failures ? 1 : 0;
}
