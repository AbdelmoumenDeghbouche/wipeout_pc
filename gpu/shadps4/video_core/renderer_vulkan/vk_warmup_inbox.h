// SPDX-License-Identifier: GPL-2.0-or-later
// gow3: helpers of the background pipeline cache warm-up (vk_pipeline_serialization.cpp).

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace Vulkan {

inline bool UseOnDemandPipelines(bool binary_cache, bool supported, bool capture_only,
                                 bool background_warmup) {
    return background_warmup && binary_cache && supported && !capture_only;
}

/// #149: set when the driver refuses a cached pipeline for lack of memory while preloading.
/// The bulk warm-up then stops reading and building: each further build would only fight the
/// game for what is left, and the rest compile when the game needs them. Results are VkResult
/// values (this header does not include Vulkan).
class WarmupMemoryStop {
public:
    static constexpr int OutOfHostMemory = -1;   ///< VK_ERROR_OUT_OF_HOST_MEMORY
    static constexpr int OutOfDeviceMemory = -2; ///< VK_ERROR_OUT_OF_DEVICE_MEMORY

    void Report(int result) {
        if (result == OutOfHostMemory || result == OutOfDeviceMemory) {
            stopped.store(true, std::memory_order_relaxed);
        }
    }
    bool Stopped() const {
        return stopped.load(std::memory_order_relaxed);
    }
    void Reset() {
        stopped.store(false, std::memory_order_relaxed);
    }

private:
    std::atomic<bool> stopped{false};
};

/// The one shared by the pipeline constructors (preloading) and the warm-ups.
inline WarmupMemoryStop& PreloadMemoryStop() {
    static WarmupMemoryStop stop;
    return stop;
}

/// Warm-up threads: one on four cores or fewer, otherwise all but two, at most six.
inline unsigned WarmupThreadCount(unsigned cores) {
    return cores <= 4 ? 1u : std::min(cores - 2, 6u);
}

/// Items made on worker threads, handed to the GPU thread a few at a time so that a frame
/// never pays for the whole cache at once.
template <typename T>
class WarmupInbox {
public:
    void Push(T item) {
        std::scoped_lock lock{mutex};
        items.push_back(std::move(item));
    }

    /// At most `max` items, oldest first.
    std::vector<T> Take(std::size_t max) {
        std::scoped_lock lock{mutex};
        const std::size_t count = std::min(max, items.size());
        std::vector<T> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(std::move(items.front()));
            items.pop_front();
        }
        return out;
    }

    bool Empty() {
        std::scoped_lock lock{mutex};
        return items.empty();
    }

private:
    std::mutex mutex;
    std::deque<T> items;
};

/// The first pipeline ready for a key is kept: a warm-up pipeline that arrives after the
/// game compiled the same one (or the reverse) is dropped. True when `value` was stored.
template <typename Map, typename Key, typename Value>
bool PublishFirst(Map& map, const Key& key, Value&& value) {
    return map.try_emplace(key, std::forward<Value>(value)).second;
}

} // namespace Vulkan
