// SPDX-License-Identifier: GPL-2.0-or-later
// GPU regression: growing mip chains retain their backing and GPU-written levels.
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>

extern "C" {
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
constexpr u64 GuestBytes = 1 << 20;
bool RunCase(const Vulkan::Instance &instance, Vulkan::Scheduler &scheduler,
             Vulkan::Runtime &runtime, VideoCore::TextureCache &cache, void *guest, u64,
             u32 layers = 1) {
    using namespace VideoCore;
    TextureCache::ImageDesc desc{};
    std::memset(guest, 0xa5, GuestBytes);
    desc.type = TextureCache::BindingType::Texture;
    auto &info = desc.info;
    info.type = AmdGpu::ImageType::Color2D;
    info.pixel_format = vk::Format::eR32Uint;
    info.size = {64, 64, 1};
    info.pitch = 64;
    info.num_bits = 32;
    info.resources = {.levels = 1, .layers = layers};
    info.guest_address = reinterpret_cast<VAddr>(guest);
    desc.view_info.type =
        layers == 1 ? AmdGpu::ImageType::Color2D : AmdGpu::ImageType::Color2DArray;
    desc.view_info.format = info.pixel_format;
    ImageId previous{};
    u64 uid = 0;
    vk::Image handle{};
    bool passed = true;
    Buffer download(instance, 0, 64 * 64 * 4, MemoryType::HostCached);
    const auto check_mip = [&](Image &image, u32 mip, u32 expected) {
        const u32 extent = 64 >> mip;
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, mip, 0, 1},
            .imageExtent = {extent, extent, 1}};
        runtime.DownloadImage(&image, &download, std::span{&copy, 1});
        runtime.AccessBuffer(&download, 0, download.SizeBytes(), vk::PipelineStageFlagBits2::eHost,
                             vk::AccessFlagBits2::eHostRead);
        runtime.FlushBarriers();
        scheduler.Finish();
        download.Invalidate(0, download.SizeBytes());
        for (u32 i = 0; i < extent * extent; i++) {
            u32 value;
            std::memcpy(&value, download.mapped_data.data() + i * 4, 4);
            if (value != expected) {
                std::printf("FAIL: %u layers mip %u pixel %u got %#x expected %#x\n", layers, mip,
                            i, value, expected);
                return false;
            }
        }
        return true;
    };
    const u32 max_levels = layers == 1 ? 7 : 3;
    for (u32 levels = 1; levels <= max_levels; ++levels) {
        info.resources.levels = levels;
        info.UpdateSize();
        desc.view_info.range.extent = info.resources;
        const auto id = cache.FindImage(desc, true);
        auto &image = cache.GetImage(id);
        cache.UpdateImage(id);
        if (layers == 1 && levels >= 3 && (image.image_uid != uid || image.GetImage() != handle)) {
            std::printf("FAIL: mip growth %u recreated backing (uid %llu -> %llu)\n", levels,
                        (unsigned long long)uid, (unsigned long long)image.image_uid);
            passed = false;
        }
        if (layers > 1 && levels >= 3 && image.GetImage() == handle)
            passed = false;
        passed = check_mip(image, levels - 1, 0xa5a5a5a5u) && passed;
        if (image.info.guest_size != info.guest_size || image.info.resources.levels != levels)
            passed = false;
        if (levels >= 2) {
            uid = image.image_uid;
            handle = image.GetImage();
        }
        previous = id;
        // Each mip becomes GPU-authoritative before the next expansion.
        vk::ClearValue clear{};
        clear.color.uint32 = std::array{0x12345678u + levels - 1, 0u, 0u, 0u};
        runtime.ClearImage(
            &image, {.base = {.level = levels - 1}, .extent = {.levels = 1, .layers = 1}}, clear);
        scheduler.Finish();
    }
    auto &image = cache.GetImage(previous);
    for (u32 mip = 0; mip < max_levels; ++mip)
        passed = check_mip(image, mip, 0x12345678u + mip) && passed;
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
        passed = RunCase(instance, scheduler, runtime, cache, guest, GuestBytes);
        cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
        scheduler.Finish();
        passed = RunCase(instance, scheduler, runtime, cache, guest, GuestBytes, 2) && passed;
        cache.UnmapMemory(reinterpret_cast<VAddr>(guest), GuestBytes);
        scheduler.Finish();
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
