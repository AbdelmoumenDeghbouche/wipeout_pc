// SPDX-License-Identifier: GPL-2.0-or-later
// GPU regression for a newer fallback copy being overwritten by an older queued copy.
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

extern "C" {
#include "runtime.h"
void runtime_memory_set_gpu_hooks(void (*map)(uintptr_t, uint64_t),
                                 void (*unmap)(uintptr_t, uint64_t),
                                 void (*invalidate)(uintptr_t, uint64_t));
// The settings menu restarts through probe.c, which this test does not link.
__attribute__((visibility("default"))) void runtime_restart(void) { std::abort(); }
// This renderer fixture maps guest data, but never loads a guest executable.
__attribute__((visibility("default"))) unsigned char* runtime_image_base(void) { return nullptr; }
}

// Keep Vulkan-Hpp's dispatcher layout consistent with the library before enabling asserts.
#undef NDEBUG
#include <cassert>

namespace {
constexpr u32 Height = 4096;
constexpr u32 Layers = 129;
constexpr u32 Older = 0x11223344;
constexpr u32 Newer = 0x55667788;
constexpr u64 QueueLimit = 64ull << 20;

bool RequiredEnvironment() {
    for (const auto& [name, expected] : std::array{
             std::pair{"GOW3_DEFERRED_READBACK", "1"},
             std::pair{"GOW3_READBACK_LINEAR", "1"},
             std::pair{"GOW3_DRAW_PIPE", "0"}}) {
        const char* value = std::getenv(name);
        if (!value || std::strcmp(value, expected)) {
            std::fprintf(stderr, "SETUP: run with %s=%s\n", name, expected);
            return false;
        }
    }
    const char* stale = std::getenv("GOW3_STALE_READBACK");
    const char* toggles = std::getenv("GOW3_TOGGLE_FILE");
    if ((stale && stale[0] == '1') || (toggles && toggles[0])) {
        std::fprintf(stderr, "SETUP: disable stale readbacks and the live toggle file\n");
        return false;
    }
    return true;
}

bool RunCase(Vulkan::Scheduler& scheduler, Vulkan::Runtime& runtime,
             VideoCore::TextureCache& cache, void* guest, u64 guest_size) {
    using namespace VideoCore;
    TextureCache::ImageDesc desc{};
    desc.type = TextureCache::BindingType::Storage;
    auto& info = desc.info;
    info.type = AmdGpu::ImageType::Color2D; // ImageInfo stores the base type, as GetBaseType().
    info.pixel_format = vk::Format::eR32Uint;
    info.size = {1, Height, 1};
    info.pitch = 1;
    info.num_bits = 32;
    info.resources = {.levels = 1, .layers = Layers};
    info.guest_address = reinterpret_cast<VAddr>(guest);
    info.UpdateSize();
    assert(info.guest_size == guest_size);
    assert(info.mips_layout[0].pitch == 16);
    const u64 estimate = u64{info.pitch} * Height * Layers * sizeof(u32);
    assert(guest_size <= QueueLimit && guest_size + estimate <= QueueLimit);
    assert(guest_size * 2 > QueueLimit);
    desc.view_info.type = AmdGpu::ImageType::Color2DArray;
    desc.view_info.format = info.pixel_format;
    desc.view_info.range.extent = info.resources;
    desc.view_info.is_storage = true;
    const ImageId id = cache.FindImage(desc, true);
    Image& image = cache.GetImage(id);
    const SubresourceRange all{.extent = info.resources};
    const auto queue_clear = [&](u32 value) {
        vk::ClearValue clear{};
        clear.color.uint32 = std::array{value, 0u, 0u, 0u};
        runtime.ClearImage(&image, all, clear);
        // Keep the earlier GPU result pending: refreshing here could resolve it first.
        (void)cache.FindTexture(id, desc, nullptr, false);
        cache.ProcessDownloadImages();
    };

    const u64 tick = scheduler.CurrentTick();
    queue_clear(Older);
    assert(scheduler.CurrentTick() == tick && !scheduler.IsFree(tick));
    queue_clear(Newer);
    // Neither clear nor a dedicated staging request submits; the actual-size fallback did.
    assert(scheduler.CurrentTick() > tick && scheduler.IsFree(tick));
    // Retire anything the fallback left queued before touching protected guest memory.
    cache.ProcessDownloadImages();

    bool passed = true;
    for (u32 layer = 0; layer < Layers && passed; ++layer) {
        for (u32 y = 0; y < Height; ++y) {
            const u64 offset = (u64{layer} * Height + y) * info.mips_layout[0].pitch * sizeof(u32);
            u32 actual;
            std::memcpy(&actual, static_cast<const u8*>(guest) + offset, sizeof(actual));
            if (actual != Newer) {
                std::fprintf(stderr, "FAIL: readback fallback layer %u row %u: expected %#x, got %#x\n",
                             layer, y, Newer, actual);
                passed = false;
                break;
            }
        }
    }
    std::printf("Readback fallback: estimate %.6f MiB, each copy %.2f MiB, combined %.2f MiB\n",
                estimate / 1048576.0, guest_size / 1048576.0, guest_size * 2 / 1048576.0);
    return passed;
}
} // namespace

int main() {
    // The real Windows runtime must reserve guest addresses before Vulkan allocates heaps.
    runtime_memory_reserve();
    if (!RequiredEnvironment()) return 2;
    runtime_thread_attach_main();
    using Allocate = int32_t (ABI *)(int64_t, int64_t, uint64_t, uint64_t, int, int64_t*);
    using Map = int32_t (ABI *)(void**, uint64_t, int, int, int64_t, uint64_t);
    using Unmap = int32_t (ABI *)(void*, uint64_t);
    using Release = int32_t (ABI *)(uint64_t, uint64_t);
    const auto allocate = reinterpret_cast<Allocate>(runtime_memory_resolve("rTXw65xmLIA#libkernel"));
    const auto map = reinterpret_cast<Map>(runtime_memory_resolve("L-Q3LEjIbgA#libkernel"));
    const auto unmap = reinterpret_cast<Unmap>(runtime_memory_resolve("cQke9UuBQOk#libkernel"));
    const auto release = reinterpret_cast<Release>(runtime_memory_resolve("MBuItvba6z8#libkernel"));
    constexpr u64 GuestBytes = u64{16} * Height * Layers * sizeof(u32);
    static_assert(GuestBytes % 16384 == 0);
    int64_t physical = -1;
    void* guest = nullptr;
    assert(allocate && map && unmap && release);
    assert(allocate(0, 512ull << 20, GuestBytes, 16384, 0, &physical) == 0);
    assert(map(&guest, GuestBytes, 3, 0, physical, 0) == 0);
    bool passed;
    {
        std::fprintf(stderr, "Fixture object sizes: Rasterizer %zu (heap), Instance %zu, "
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
        auto& rasterizer = *rasterizer_storage;
        auto& cache = rasterizer.GetTextureCache();
        passed = RunCase(scheduler, runtime, cache, guest, GuestBytes);
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
    std::puts(passed ? "PASS: newer fallback readback survives retirement of its older copy"
                     : "FAIL: older deferred readback overwrote its newer fallback copy");
    return passed ? 0 : 1;
}
