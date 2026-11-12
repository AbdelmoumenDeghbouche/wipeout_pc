// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "gow3_toggles.h"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cstdlib>
#include <cstring>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/types.h"
#include "core/devtools/widget/gpu_memory.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"

namespace Vulkan {

using VideoCore::MemoryType;

namespace {

// gow3: staging memory is populated (allocated and cleared) by the kernel on first CPU access,
// ~2 ms per 16 MiB block. Streaming bursts a few seconds apart re-created the blocks trimmed
// in between and copies into them ran at 0.3 GB/s (stutter). Blocks now live 30 s idle, the
// upload ring keeps GOW3_STAGING_KEEP_MB (512) and is populated at startup.
constexpr u64 RING_IDLE_FRAMES = 3000;
constexpr u64 LARGE_IDLE_FRAMES = 3000;
// A track load uploads its big textures through one device and one host buffer each (about
// 900 MiB of each in a WipEout HD race); kept 3000 frames, they pushed 4-8 GB cards out of
// video memory right after the load (#72). With memory low they go after 2 s at 60 FPS.
constexpr u64 LOW_MEMORY_IDLE_FRAMES = 120;
constexpr u64 PREWARM_BLOCKS = 8;
constexpr u64 LARGE_MIN_GRANULARITY = 64_KB;
constexpr u64 BLOCK_SIZE = 16_MB;
// gow3: no large upload for this long and a load is over: idle large buffers all go.
constexpr auto QUIET_TIME = std::chrono::seconds(5);

u64 RoundAllocationSize(u64 size) {
    const u64 step = std::max(std::bit_floor(size) / 4, LARGE_MIN_GRANULARITY);
    return Common::AlignUp(size, step);
}

} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Instance& instance_, Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {
    const char* env = std::getenv("GOW3_STAGING_KEEP_MB");
    keep_blocks = (env ? std::strtoull(env, nullptr, 10) : 512) * 1_MB / BLOCK_SIZE;
    Ring& ring = rings[u32(MemoryType::HostUncached)];
    // gow3: every block the ring may keep is made and written here. Only PREWARM_BLOCKS were,
    // so the first race made the rest in its first frame, where the kernel populated them
    // under the texture uploads (a longer freeze). With memory low TrimRing still takes the
    // ring back to PREWARM_BLOCKS.
    for (u64 i = 0; i < std::max(keep_blocks, PREWARM_BLOCKS); ++i) {
        auto& block = ring.blocks.emplace_back(Block{
            .buffer = std::make_unique<VideoCore::StreamBuffer>(
                instance, scheduler, MemoryType::HostUncached, BLOCK_SIZE),
            .last_used_frame = frame,
            .last_used_time = std::chrono::steady_clock::now(),
        });
        if (!block.buffer->mapped_data.empty()) {
            std::memset(block.buffer->mapped_data.data(), 0, block.buffer->mapped_data.size());
        }
    }
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(u64 size, MemoryType type, u64 alignment,
                                            bool deferred) {
    Ring& ring = rings[u32(type)];
    if (deferred || size > BLOCK_SIZE) {
        return RequestLarge(size, type, deferred);
    }
    return RequestFromRing(ring, size, alignment, type);
}

StagingBufferRef StagingBufferPool::RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                                    MemoryType type) {
    Block* best{};
    u64 best_offset{};
    // gow3: the current block, then the lowest one with room. Going on round the ring from the
    // current one used every block in turn: none was ever idle, so the ~90 blocks track loads
    // made (1.4 GB) all stayed for good.
    for (size_t i = 0; i <= ring.blocks.size(); ++i) {
        const size_t index = i == 0 ? ring.current : i - 1;
        if (index >= ring.blocks.size() || (i != 0 && index == ring.current)) {
            continue;
        }
        Block* block = &ring.blocks[index];
        if (const auto offset = block->buffer->Reserve(size, alignment, false)) {
            ring.current = index;
            best = block;
            best_offset = *offset;
            break;
        }
    }

    if (!best) {
        best = &ring.blocks.emplace_back(Block{
            .buffer =
                std::make_unique<VideoCore::StreamBuffer>(instance, scheduler, type, BLOCK_SIZE),
            .last_used_frame = frame,
            .last_used_time = std::chrono::steady_clock::now(),
        });
        ring.current = ring.blocks.size() - 1;
        best_offset = *best->buffer->Reserve(size, alignment, false);
    }

    auto& buffer = *best->buffer;
    best->last_used_frame = frame;
    best->last_used_time = std::chrono::steady_clock::now();

    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = best_offset,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data() + best_offset;
    }
    return ref;
}

StagingBufferRef StagingBufferPool::RequestLarge(u64 size, MemoryType type, bool deferred) {
    auto& cache = large_caches[u32(type)];
    const u64 max_size = std::max(size + size / 4, RoundAllocationSize(size));

    LargeBuffer* best{};
    for (LargeBuffer& entry : cache) {
        const u64 entry_size = entry.buffer->SizeBytes();
        if (entry.held || entry_size < size || entry_size > max_size ||
            !scheduler.IsFree(entry.tick)) {
            continue;
        }
        if (!best || entry_size < best->buffer->SizeBytes()) {
            best = &entry;
        }
    }

    if (!best) {
        // gow3: no idle buffer fits, so the idle ones are all of other sizes: they go before a
        // new one is made. Kept for reuse, a quick track load made 3-5 GB of them (loads ask
        // for textures of many sizes) and Windows ran out of commit (RAM and page file,
        // 1.3 GB left). Now the cache holds what the GPU still reads, plus this one.
        std::erase_if(cache, [this](const LargeBuffer& entry) {
            return !entry.held && scheduler.IsFree(entry.tick);
        });
        // gow3: only a request that needs a new buffer counts as loading (TrimLarge). A race
        // reuses one now and then, which kept a finished load's 4.6 GB for a whole race.
        last_large_request[u32(type)] = std::chrono::steady_clock::now();
        best = &cache.emplace_back(LargeBuffer{
            .buffer = std::make_unique<VideoCore::Buffer>(instance, 0, RoundAllocationSize(size),
                                                          type, "StagingBufferPool:Dedicated"),
            .tick = 0,
            .last_used_frame = frame,
            .held = false,
        });
    }

    best->tick = scheduler.CurrentTick();
    best->last_used_frame = frame;
    best->held = deferred;

    VideoCore::Buffer& buffer = *best->buffer;
    StagingBufferRef ref{
        .buffer = &buffer,
        .offset = 0,
        .size = size,
        .type = type,
    };
    if (!buffer.mapped_data.empty()) {
        ref.mapped = buffer.mapped_data.data();
    }
    return ref;
}

void StagingBufferPool::FreeDeferred(const StagingBufferRef& ref) {
    auto& cache = large_caches[u32(ref.type)];
    const auto it = std::ranges::find_if(
        cache, [&](const LargeBuffer& entry) { return entry.buffer.get() == ref.buffer; });
    ASSERT_MSG(it != cache.end() && it->held, "FreeDeferred on an allocation that is not held");
    it->held = false;
    it->tick = scheduler.CurrentTick();
    it->last_used_frame = frame;
}

void StagingBufferPool::TickFrame() {
    ++frame;
    for (Ring& ring : rings) {
        TrimRing(ring);
    }
    for (auto& cache : large_caches) {
        TrimLarge(cache);
    }
    PublishStats();
}

void StagingBufferPool::TrimRing(Ring& ring) {
    // gow3: only the upload ring keeps blocks (keep_blocks). The device ring kept them too:
    // 640 MiB of video memory after a 2048 track load, for good. With memory low the upload
    // ring keeps its prewarmed blocks and idle blocks of any ring go after 2 s at 60 FPS.
    const bool upload = &ring == &rings[u32(MemoryType::HostUncached)];
    const u64 keep = !upload ? 0 : low_memory ? std::min(keep_blocks, PREWARM_BLOCKS) : keep_blocks;
    const u64 idle_frames = low_memory ? LOW_MEMORY_IDLE_FRAMES : RING_IDLE_FRAMES;
    auto& blocks = ring.blocks;
    const auto now = std::chrono::steady_clock::now();
    for (size_t i = blocks.size(); i-- > 0;) {
        const Block& block = blocks[i];
        // gow3: or unused for QUIET_TIME. Blocks fill lowest first, so those above the kept
        // ones serve loads only; races tick too rarely for 3000 frames to pass (1.2 GB kept).
        const bool idle = frame - block.last_used_frame >= idle_frames ||
                          now - block.last_used_time >= QUIET_TIME;
        if (blocks.size() <= keep || !idle || !scheduler.IsFree(block.buffer->LastTick())) {
            continue;
        }
        blocks.erase(blocks.begin() + i);
        if (i < ring.current) {
            --ring.current;
        } else if (i == ring.current) {
            ring.current = 0;
        }
    }
    if (ring.current >= blocks.size()) {
        ring.current = 0;
    }
}

void StagingBufferPool::TrimLarge(std::vector<LargeBuffer>& cache) {
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held &&
               frame - entry.last_used_frame >= (low_memory ? LOW_MEMORY_IDLE_FRAMES : LARGE_IDLE_FRAMES) &&
               scheduler.IsFree(entry.tick);
    });
    // gow3: and all of them once no new one was needed for QUIET_TIME (a track load is over). Loads ask for textures of many sizes, a buffer serves
    // only up to 1.25x its size, and loads ~20 s apart never let one sit 3000 frames: 3-4 GB
    // more per load, 11 GB after five.
    const auto type = static_cast<size_t>(&cache - large_caches.data());
    if (std::chrono::steady_clock::now() - last_large_request[type] < QUIET_TIME) {
        return;
    }
    std::erase_if(cache, [this](const LargeBuffer& entry) {
        return !entry.held && scheduler.IsFree(entry.tick);
    });
}

void StagingBufferPool::PublishStats() const {
    using namespace Core::Devtools::Widget;
    if (Gow3Stats::enabled) {
        u64 bytes = 0;
        for (size_t i = 0; i < NUM_TYPES; ++i) {
            u64 ring_bytes = 0, large_bytes = 0;
            for (const Block& block : rings[i].blocks) {
                ring_bytes += block.buffer->SizeBytes();
            }
            for (const LargeBuffer& entry : large_caches[i]) {
                large_bytes += entry.buffer->SizeBytes();
            }
            Gow3Stats::staging_mib[i][0] = static_cast<s64>(ring_bytes >> 20);
            Gow3Stats::staging_mib[i][1] = static_cast<s64>(large_bytes >> 20);
            bytes += ring_bytes + large_bytes;
        }
        Gow3Stats::staging_bytes = static_cast<s64>(bytes);
    }
    if (!GpuMemoryViewer::IsEnabled()) {
        return;
    }
    static constexpr std::array<const char*, NUM_TYPES> names = {"DeviceLocal", "HostUncached",
                                                                 "HostCached"};
    GpuMemoryGroup group{.name = "Staging"};
    for (size_t i = 0; i < NUM_TYPES; ++i) {
        u64 bytes = 0;
        for (const Block& block : rings[i].blocks) {
            bytes += block.buffer->SizeBytes();
        }
        for (const LargeBuffer& entry : large_caches[i]) {
            bytes += entry.buffer->SizeBytes();
        }
        if (bytes != 0) {
            group.rows.push_back({.name = names[i], .bytes = bytes});
        }
    }
    GpuMemoryViewer::Publish(std::move(group));
}

} // namespace Vulkan
