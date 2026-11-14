// SPDX-License-Identifier: GPL-2.0-or-later
// GPU regression: incremental uploads preserve old levels and copy only the new range.
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

namespace {
std::atomic<bool> watch_copies{false};
std::atomic<u64> copy_floor{0}, copies_seen{0}, wrong_copy_ranges{0};
} // namespace

extern "C" {
// Observe guest-to-staging reads without adding production instrumentation.
int __real_runtime_memory_region(uintptr_t, uintptr_t *, uintptr_t *, int *);
int __wrap_runtime_memory_region(uintptr_t address, uintptr_t *start, uintptr_t *end, int *mapped) {
    if (watch_copies.load()) {
        ++copies_seen;
        if (address < copy_floor.load())
            ++wrong_copy_ranges;
    }
    return __real_runtime_memory_region(address, start, end, mapped);
}
#include "runtime.h"
int runtime_wipeout_paused(void) { return 0; }
void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t),
                                  void (*unmap)(uintptr_t, uint64_t),
                                  void (*invalidate)(uintptr_t, uint64_t));
// The settings menu restarts through probe.c, which this test does not link.
__attribute__((visibility("default"))) void runtime_restart(void) { std::abort(); }
// This renderer fixture maps guest data, but never loads a guest executable.
__attribute__((visibility("default"))) unsigned char *runtime_image_base(void) { return nullptr; }
}

// Keep Vulkan-Hpp's dispatcher layout consistent with the library before enabling asserts.
#undef NDEBUG
#include <cassert>

namespace {
constexpr u64 GuestBytes = 16 << 20;
bool RunCase(const Vulkan::Instance &instance, Vulkan::Scheduler &scheduler,
             Vulkan::Runtime &runtime, VideoCore::TextureCache &cache, void *guest, u32 layers = 1,
             bool tiled = false, bool pro = false, u32 bits = 32, bool jumps = false) {
    using namespace VideoCore;
    TextureCache::ImageDesc desc{};
    std::memset(guest, 0xcd, GuestBytes);
    desc.type = TextureCache::BindingType::Texture;
    auto &info = desc.info;
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = bits == 64 ? vk::Format::eR32G32Uint : vk::Format::eR32Uint;
    const u32 side = tiled ? 1024 : 64;
    info.size = {side, side, 1};
    info.pitch = side;
    info.num_bits = bits;
    info.resources = {.levels = 1, .layers = layers};
    info.tile_mode = tiled ? AmdGpu::TileMode::Thin2DThin : AmdGpu::TileMode::DisplayLinearAligned;
    info.array_mode = AmdGpu::GetArrayMode(info.tile_mode);
    info.props.is_tiled = tiled;
    info.alt_tile = tiled && pro;
    info.bank_swizzle = tiled ? 3 : 0;
    info.guest_address = reinterpret_cast<VAddr>(guest);
    const u32 max_levels = layers == 1 ? (tiled ? 11 : 7) : 3;
    auto full = info;
    full.resources.levels = max_levels;
    full.UpdateSize();
    assert(full.guest_size < GuestBytes);
    for (u32 mip = 0; mip < max_levels; mip++) {
        const auto &layout = full.mips_layout[mip];
        auto *words = reinterpret_cast<u32 *>(static_cast<u8 *>(guest) + layout.offset);
        std::fill_n(words, layout.size / 4, 0xa5000000u + mip);
    }
    desc.view_info.type =
        layers == 1 ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DArray;
    desc.view_info.format = info.pixel_format;
    ImageId previous{};
    u64 uid = 0;
    vk::Image handle{};
    bool passed = true;
    Buffer download(instance, 0, side * side * (bits / 8), MemoryType::HostCached);
    const auto check_mip = [&](Image &image, u32 mip, u32 expected, bool gpu_written = false) {
        const u32 extent = side >> mip;
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
            .imageExtent = {extent, extent, 1}};
        runtime.DownloadImage(&image, &download, std::span{&copy, 1});
        runtime.AccessBuffer(&download, 0, download.SizeBytes(), vk::PipelineStageFlagBits2::eHost,
                             vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        download.Invalidate(0, download.SizeBytes());
        for (u32 i = 0; i < extent * extent * (bits / 32); i++) {
            u32 value;
            std::memcpy(&value, download.mapped_data.data() + i * 4, 4);
            const u32 expected_word = gpu_written && i % (bits / 32) != 0 ? 0 : expected;
            if (value != expected_word) {
                std::printf("FAIL: %u layers mip %u pixel %u got %#x expected %#x\n", layers, mip,
                            i, value, expected);
                return false;
            }
        }
        return true;
    };
    std::vector<u32> steps;
    for (u32 levels = 1; levels <= max_levels; ++levels)
        if (!jumps || levels == 1 || levels == 2 || levels == 4 || levels == 7 || levels == 11)
            steps.push_back(levels);
    u32 previous_levels = 0;
    for (u32 levels : steps) {
        info.resources.levels = levels;
        info.UpdateSize();
        desc.view_info.range.extent = info.resources;
        // Individual growth of these linear/base/Pro cases must never reread older mips.
        const bool watch = layers == 1 && levels >= 3 && !jumps;
        copy_floor = info.guest_address + info.mips_layout[levels - 1].offset;
        copies_seen = wrong_copy_ranges = 0;
        watch_copies = watch;
        const auto id = cache.FindImage(desc, true);
        auto &image = cache.GetImage(id);
        cache.UpdateImage(id); // FindImage alone leaves the first image dirty.
        scheduler.Finish();
        watch_copies = false;
        if (watch && (copies_seen == 0 || wrong_copy_ranges != 0)) {
            std::printf("FAIL: mip %u reread older guest data (%llu / %llu reads)\n", levels - 1,
                        (unsigned long long)wrong_copy_ranges.load(),
                        (unsigned long long)copies_seen.load());
            passed = false;
        }
        std::printf(
            "MIP_CASE layers=%u levels=%u uid=%llu guest_size=%u new_offset=%u new_size=%u\n",
            layers, levels, (unsigned long long)image.image_uid, info.guest_size,
            info.mips_layout[levels - 1].offset, info.mips_layout[levels - 1].size);
        if (layers == 1 && levels >= 3 && (image.image_uid != uid || image.GetImage() != handle)) {
            std::printf("FAIL: mip growth %u recreated backing (uid %llu -> %llu)\n", levels,
                        (unsigned long long)uid, (unsigned long long)image.image_uid);
            passed = false;
        }
        if (layers > 1 && levels >= 3 && image.GetImage() == handle)
            passed = false;
        for (u32 mip = previous_levels; mip < levels; ++mip)
            passed = check_mip(image, mip, 0xa5000000u + mip) && passed;
        if (image.info.guest_size != info.guest_size || image.info.resources.levels != levels)
            passed = false;
        if (levels >= 2) {
            uid = image.image_uid;
            handle = image.GetImage();
        }
        previous = id;
        // Each new mip becomes GPU-authoritative before the next expansion.
        for (u32 mip = previous_levels; mip < levels; ++mip) {
            vk::ClearValue clear{};
            clear.color.uint32 = std::array{0x12345678u + mip, 0u, 0u, 0u};
            runtime.ClearImage(
                &image, {.base = {.level = mip}, .extent = {.levels = 1, .layers = 1}}, clear);
        }
        previous_levels = levels;
        scheduler.Finish();
    }
    auto &image = cache.GetImage(previous);
    for (u32 mip = 0; mip < max_levels; ++mip)
        passed = check_mip(image, mip, 0x12345678u + mip, true) && passed;
    return passed;
}
} // namespace

int main() {
    // The real Windows runtime must reserve guest addresses before Vulkan allocates heaps.
    runtime_memory_reserve();
    runtime_thread_attach_main();
    using Allocate = int32_t(ABI *)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t *);
    using Map = int32_t(ABI *)(void **, uint64_t, int, int, int64_t, uint64_t);
    using Unmap = int32_t(ABI *)(void *, uint64_t);
    using Release = int32_t(ABI *)(uint64_t, uint64_t);
    const auto allocate =
        reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#libkernel"));
    const auto map = reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#libkernel"));
    const auto unmap = reinterpret_cast<Unmap>(runtime_memory_resolve("cQke9UuBQOk#libkernel"));
    const auto release = reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#libkernel"));

    static_assert(GuestBytes % 16384 == 0);
    int64_t physical = -1;
    void *guest = nullptr;
    assert(allocate && map && unmap && release);
    assert(allocate(0, 512ull << 20, GuestBytes, 16384, 0, &physical) == 0);
    assert(map(&guest, GuestBytes, 3, 0, physical, 0) == 0);
    bool passed;
    {
        std::fprintf(stderr,
                     "Fixture object sizes: Rasterizer %zu (heap), Instance %zu, "
                     "Scheduler %zu, Runtime %zu, Liverpool %zu bytes\n",
                     sizeof(Vulkan::Rasterizer), sizeof(Vulkan::Instance),
                     sizeof(Vulkan::Scheduler), sizeof(Vulkan::Runtime), sizeof(AmdGpu::Liverpool));
        Vulkan::Instance instance(0, true);
        Vulkan::Scheduler scheduler(instance);
        Vulkan::Runtime runtime(instance, scheduler);
        AmdGpu::Liverpool liverpool;
        // Keep the large inline texture caches off the default Windows stack.
        auto rasterizer_storage =
            std::make_unique<Vulkan::Rasterizer>(instance, scheduler, runtime, &liverpool);
        auto &rasterizer = *rasterizer_storage;
        auto &cache = rasterizer.GetTextureCache();
        passed = RunCase(instance, scheduler, runtime, cache, guest);
        cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
        scheduler.Finish();
        passed = RunCase(instance, scheduler, runtime, cache, guest, 2) && passed;
        cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
        scheduler.Finish();
        for (bool pro : {false, true}) {
            passed = RunCase(instance, scheduler, runtime, cache, guest, 1, true, pro) && passed;
            cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
            scheduler.Finish();
            passed =
                RunCase(instance, scheduler, runtime, cache, guest, 1, true, pro, 64) && passed;
            cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
            scheduler.Finish();
        }
        for (bool pro : {false, true})
            for (u32 bits : {32u, 64u}) {
                passed =
                    RunCase(instance, scheduler, runtime, cache, guest, 1, true, pro, bits, true) &&
                    passed;
                cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
                scheduler.Finish();
            }
        // With a 1 MiB submission budget, a 2 MiB batch must submit before its final upload.
        const u64 batch_tick = scheduler.CurrentTick();
        for (u32 item = 0; item < 8; ++item) {
            VideoCore::TextureCache::ImageDesc desc{};
            desc.type = VideoCore::TextureCache::BindingType::Texture;
            auto &info = desc.info;
            info.type = AmdGpu::ImageType::Color2D;
            info.pixel_format = vk::Format::eR32Uint;
            info.size = {256, 256, 1};
            info.pitch = 256;
            info.num_bits = 32;
            info.resources = {.levels = 1, .layers = 1};
            info.guest_address = reinterpret_cast<VAddr>(guest) + (item << 20);
            info.UpdateSize();
            assert(info.guest_size == (256u << 10));
            const auto id = cache.FindImage(desc, true);
            cache.UpdateImage(id);
        }
        assert(scheduler.CurrentTick() > batch_tick);
        std::puts("PASS: 2 MiB batch crossed the 1 MiB staging submission budget");
        scheduler.Finish();
        cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
        scheduler.Finish();
        auto &staging = runtime.GetStagingPool();
        for (u64 size : {17ull << 20, 20ull << 20, 65536ull}) {
            const auto ref = staging.Request(size, VideoCore::MemoryType::HostUncached,
                                             instance.StorageMinAlignment());
            const auto charge =
                staging.UploadReservationBytes(size, instance.StorageMinAlignment());
            assert(charge >= ref.size);
            if (size > (16ull << 20))
                assert(charge >= ref.buffer->SizeBytes());
            std::printf("STAGING_BUDGET requested=%llu allocated=%llu charge=%llu\n",
                        (unsigned long long)size, (unsigned long long)ref.buffer->SizeBytes(),
                        (unsigned long long)charge);
            scheduler.Finish();
        }
        // No loader owns teardown here: disconnect callbacks before their owners die.
        scheduler.SetSubmitCallback({});
        runtime.SetImageAccessHook(nullptr, nullptr);
        liverpool.BindRasterizer(nullptr);
        runtime_memory_set_gpu_hooks(nullptr, nullptr, nullptr);
    }
    assert(unmap(guest, GuestBytes) == 0);
    assert(release(physical, GuestBytes) == 0);
    std::puts(passed ? "PASS: mip growth reuses its backing and preserves GPU-written pixels"
                     : "FAIL: mip growth");
    return passed ? 0 : 1;
}
