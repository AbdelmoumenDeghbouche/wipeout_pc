// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <memory>
#include <vector>

#include "common/types.h"
#include "video_core/buffer_cache/buffer.h"

namespace Vulkan {

class Instance;
class Scheduler;

struct StagingBufferRef {
    VideoCore::Buffer* buffer{};
    u64 offset{};
    u64 size{};
    u8* mapped{};
    VideoCore::MemoryType type{};

    void Flush() const noexcept {
        buffer->Flush(offset, size);
    }

    void Invalidate() const noexcept {
        buffer->Invalidate(offset, size);
    }
};

class StagingBufferPool {
public:
    explicit StagingBufferPool(const Instance& instance, Scheduler& scheduler);
    ~StagingBufferPool();

    StagingBufferPool(const StagingBufferPool&) = delete;
    StagingBufferPool& operator=(const StagingBufferPool&) = delete;

    /// Requests size bytes of memory of the given type.
    [[nodiscard]] StagingBufferRef Request(u64 size, VideoCore::MemoryType type, u64 alignment = 0,
                                           bool deferred = false);

    /// Releases a deferred allocation. It becomes reusable once the current tick completes.
    void FreeDeferred(const StagingBufferRef& ref);

    /// Releases idle memory.
    void TickFrame();

    /// gow3: set by the texture cache while video memory is low: large buffers then go after
    /// LOW_MEMORY_IDLE_FRAMES instead of 3000 frames.
    void SetLowMemory(bool low) {
        low_memory = low;
    }

private:
    struct Block {
        std::unique_ptr<VideoCore::StreamBuffer> buffer;
        u64 last_used_frame;
    };

    struct Ring {
        std::vector<Block> blocks;
        size_t current = 0;
    };

    struct LargeBuffer {
        std::unique_ptr<VideoCore::Buffer> buffer;
        u64 tick;
        u64 last_used_frame;
        bool held;
    };

    static constexpr size_t NUM_TYPES = 3;
    bool low_memory = false;

    StagingBufferRef RequestFromRing(Ring& ring, u64 size, u64 alignment,
                                     VideoCore::MemoryType type);
    StagingBufferRef RequestLarge(u64 size, VideoCore::MemoryType type, bool deferred);

    void TrimRing(Ring& ring);
    void TrimLarge(std::vector<LargeBuffer>& cache);

    void PublishStats() const;

    const Instance& instance;
    Scheduler& scheduler;

    std::array<Ring, NUM_TYPES> rings;
    u64 keep_blocks = 0; ///< gow3: ring blocks never trimmed
    std::array<std::vector<LargeBuffer>, NUM_TYPES> large_caches;
    u64 frame = 0;
};

} // namespace Vulkan
